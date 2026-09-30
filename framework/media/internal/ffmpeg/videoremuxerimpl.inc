/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//! NOTE Included inside each muse::media::ffmpeg::vN namespace (see vN/videoremuxer.cpp),
//! after that version's FFmpeg headers and FFmpegLibHandler

bool VideoRemuxer::load(const FFmpegLibPaths& paths)
{
    auto handler = std::make_shared<FFmpegLibHandler>();
    if (!handler->loadLib(paths.avUtilPath, paths.avCodecPath, paths.avFormatPath, paths.swScalePath, paths.swResamplePath)
        || !handler->loadApi()) {
        LOGW() << "FFmpeg libraries not found";
        return false;
    }

    m_ff = std::move(handler);
    return true;
}

Ret VideoRemuxer::remux(const io::path_t& videoSource, const io::path_t& audioSource, const io::path_t& destination)
{
    if (!m_ff) {
        return make_ret(Ret::Code::InternalError, std::string("FFmpeg isn't loaded"));
    }

    FFmpegLibHandler& ff = *m_ff;

    AVFormatContext* videoIn = nullptr;
    AVFormatContext* audioIn = nullptr;
    AVFormatContext* out = nullptr;
    AVPacket* packet = nullptr;

    DEFER {
        if (packet) {
            ff.av_packet_free(&packet);
        }
        if (videoIn) {
            ff.avformat_close_input(&videoIn);
        }
        if (audioIn) {
            ff.avformat_close_input(&audioIn);
        }
        if (out) {
            if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
                ff.avio_close(out->pb);
            }
            ff.avformat_free_context(out);
        }
    };

    auto error = [](const std::string& text) {
        LOGE() << text;
        return make_ret(Ret::Code::UnknownError, text);
    };

    if (ff.avformat_open_input(&videoIn, videoSource.c_str(), nullptr, nullptr) < 0
        || ff.avformat_find_stream_info(videoIn, nullptr) < 0) {
        return error("unable to read the video " + videoSource.toStdString());
    }

    if (ff.avformat_open_input(&audioIn, audioSource.c_str(), nullptr, nullptr) < 0
        || ff.avformat_find_stream_info(audioIn, nullptr) < 0) {
        return error("unable to read the audio " + audioSource.toStdString());
    }

    const int videoIndex = ff.av_find_best_stream(videoIn, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = ff.av_find_best_stream(audioIn, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoIndex < 0 || audioIndex < 0) {
        return error("no video stream in the video, or no audio stream in the audio");
    }

    AVStream* videoInStream = videoIn->streams[videoIndex];
    AVStream* audioInStream = audioIn->streams[audioIndex];

    if (ff.avformat_alloc_output_context2(&out, nullptr, nullptr, destination.c_str()) < 0 || !out) {
        return error("unsupported output format for " + destination.toStdString());
    }

    AVStream* videoOutStream = ff.avformat_new_stream(out, nullptr);
    AVStream* audioOutStream = ff.avformat_new_stream(out, nullptr);
    if (!videoOutStream || !audioOutStream
        || ff.avcodec_parameters_copy(videoOutStream->codecpar, videoInStream->codecpar) < 0
        || ff.avcodec_parameters_copy(audioOutStream->codecpar, audioInStream->codecpar) < 0) {
        return error("unable to create the output streams");
    }

    // Let the muxer pick the tags valid for its container
    videoOutStream->codecpar->codec_tag = 0;
    audioOutStream->codecpar->codec_tag = 0;
    videoOutStream->time_base = videoInStream->time_base;
    audioOutStream->time_base = audioInStream->time_base;

    if (!(out->oformat->flags & AVFMT_NOFILE) && ff.avio_open(&out->pb, destination.c_str(), AVIO_FLAG_WRITE) < 0) {
        return error("unable to create " + destination.toStdString());
    }

    if (ff.avformat_write_header(out, nullptr) < 0) {
        //! NOTE Typically a video codec the container doesn't accept
        return error("the video's codec can't be stored in " + destination.toStdString());
    }

    //! NOTE The audio starts with the video's first frame, whose timestamp may not be 0
    const int64_t videoStart = videoInStream->start_time != AV_NOPTS_VALUE ? videoInStream->start_time : 0;
    const int64_t audioShift = ff.av_rescale_q(videoStart, videoInStream->time_base, audioInStream->time_base);

    packet = ff.av_packet_alloc();
    if (!packet) {
        return error("out of memory");
    }

    // Reads the next packet of the wanted stream, false at the end
    auto readNext = [&ff, packet](AVFormatContext* in, int streamIndex) {
        while (ff.av_read_frame(in, packet) >= 0) {
            if (packet->stream_index == streamIndex) {
                return true;
            }
            ff.av_packet_unref(packet);
        }
        return false;
    };

    // Interleave both inputs by time: the next packet written is always from the one that is behind
    bool videoEnded = false;
    bool audioEnded = false;
    double lastVideoSecs = 0.0;
    double lastAudioSecs = 0.0;

    auto packetSecs = [](const AVPacket* p, AVRational timeBase, double fallback) {
        const int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        return ts != AV_NOPTS_VALUE ? ts * av_q2d(timeBase) : fallback;
    };

    while (!videoEnded || !audioEnded) {
        const bool writeVideo = !videoEnded && (audioEnded || lastVideoSecs <= lastAudioSecs);

        if (writeVideo) {
            if (!readNext(videoIn, videoIndex)) {
                videoEnded = true;
                continue;
            }

            lastVideoSecs = packetSecs(packet, videoInStream->time_base, lastVideoSecs);
            ff.av_packet_rescale_ts(packet, videoInStream->time_base, videoOutStream->time_base);
            packet->stream_index = videoOutStream->index;
        } else {
            if (!readNext(audioIn, audioIndex)) {
                audioEnded = true;
                continue;
            }

            if (packet->pts != AV_NOPTS_VALUE) {
                packet->pts += audioShift;
            }
            if (packet->dts != AV_NOPTS_VALUE) {
                packet->dts += audioShift;
            }

            lastAudioSecs = packetSecs(packet, audioInStream->time_base, lastAudioSecs);
            ff.av_packet_rescale_ts(packet, audioInStream->time_base, audioOutStream->time_base);
            packet->stream_index = audioOutStream->index;
        }

        packet->pos = -1;

        // av_interleaved_write_frame takes the packet's reference over
        if (ff.av_interleaved_write_frame(out, packet) < 0) {
            return error("unable to write to " + destination.toStdString());
        }
    }

    if (ff.av_write_trailer(out) < 0) {
        return error("unable to finish " + destination.toStdString());
    }

    LOGI() << "remuxed " << videoSource << " with " << audioSource << " into " << destination;
    return make_ok();
}
