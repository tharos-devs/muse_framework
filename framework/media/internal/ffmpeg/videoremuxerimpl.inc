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

Ret VideoRemuxer::remux(const io::path_t& videoSource, const io::path_t& audioSource, const io::path_t& destination,
                        const Options& options)
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

    const AVRational videoTb = videoInStream->time_base;
    const AVRational audioTb = audioInStream->time_base;

    auto secsToTs = [](double secs, AVRational tb) {
        return static_cast<int64_t>(std::llround(secs / av_q2d(tb)));
    };

    //! NOTE Output time = input time - shift. The video's time 0 is its first frame, whose timestamp may not be 0
    const int64_t videoFirst = videoInStream->start_time != AV_NOPTS_VALUE ? videoInStream->start_time : 0;
    const int64_t audioFirst = audioInStream->start_time != AV_NOPTS_VALUE ? audioInStream->start_time : 0;
    const int64_t videoShift = videoFirst + secsToTs(options.videoStartSecs, videoTb);
    const int64_t audioShift = audioFirst + secsToTs(options.audioPrimingSecs, audioTb);

    //! NOTE Starting inside the video: from the keyframe before, the frames before the start get negative
    //! timestamps, so they're decoded but not shown (edit list)
    if (options.videoStartSecs > 0.0) {
        ff.av_seek_frame(videoIn, videoIndex, videoShift, AVSEEK_FLAG_BACKWARD);
    }

    const bool hasDuration = options.durationSecs > 0.0;

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

    auto shift = [](AVPacket* p, int64_t by) {
        if (p->pts != AV_NOPTS_VALUE) {
            p->pts -= by;
        }
        if (p->dts != AV_NOPTS_VALUE) {
            p->dts -= by;
        }
    };

    auto packetSecs = [](const AVPacket* p, AVRational timeBase, double fallback) {
        const int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        return ts != AV_NOPTS_VALUE ? ts * av_q2d(timeBase) : fallback;
    };

    auto presentationSecs = [](const AVPacket* p, AVRational timeBase) {
        const int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        return ts != AV_NOPTS_VALUE ? ts * av_q2d(timeBase) : 0.0;
    };

    //! NOTE Frames displayed before the end can be stored after it (B-frames): keep reading a little past it
    constexpr double REORDER_MARGIN_SECS = 1.0;

    // Interleave both inputs by (output) time: the next packet written is always from the one that is behind
    bool videoEnded = false;
    bool audioEnded = false;
    double lastVideoSecs = -1e9;
    double lastAudioSecs = -1e9;

    while (!videoEnded || !audioEnded) {
        const bool takeVideo = !videoEnded && (audioEnded || lastVideoSecs <= lastAudioSecs);

        if (takeVideo) {
            if (!readNext(videoIn, videoIndex)) {
                videoEnded = true;
                continue;
            }

            shift(packet, videoShift);
            lastVideoSecs = packetSecs(packet, videoTb, lastVideoSecs);

            if (hasDuration) {
                if (lastVideoSecs > options.durationSecs + REORDER_MARGIN_SECS) {
                    ff.av_packet_unref(packet);
                    videoEnded = true;
                    continue;
                }
                if (presentationSecs(packet, videoTb) >= options.durationSecs) {
                    ff.av_packet_unref(packet);
                    continue;
                }
            }

            ff.av_packet_rescale_ts(packet, videoTb, videoOutStream->time_base);
            packet->stream_index = videoOutStream->index;
        } else {
            if (!readNext(audioIn, audioIndex)) {
                audioEnded = true;
                continue;
            }

            shift(packet, audioShift);
            lastAudioSecs = packetSecs(packet, audioTb, lastAudioSecs);

            if (hasDuration && presentationSecs(packet, audioTb) >= options.durationSecs) {
                ff.av_packet_unref(packet);
                audioEnded = true;
                continue;
            }

            ff.av_packet_rescale_ts(packet, audioTb, audioOutStream->time_base);
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
