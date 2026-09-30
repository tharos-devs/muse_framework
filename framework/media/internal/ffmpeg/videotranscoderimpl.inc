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

//! NOTE Included inside each muse::media::ffmpeg::vN namespace (see vN/videotranscoder.cpp),
//! after that version's FFmpeg headers, FFmpegLibHandler and VideoDecoder

bool VideoTranscoder::load(const FFmpegLibPaths& paths)
{
    auto handler = std::make_shared<FFmpegLibHandler>();
    if (!handler->loadLib(paths.avUtilPath, paths.avCodecPath, paths.avFormatPath, paths.swScalePath, paths.swResamplePath)
        || !handler->loadApi()) {
        LOGW() << "FFmpeg libraries not found";
        return false;
    }

    //! NOTE Without an H.264 encoder (e.g. an FFmpeg built without any), this can't do anything
    if (!handler->avcodec_find_encoder(AV_CODEC_ID_H264)) {
        LOGW() << "no H.264 encoder in the FFmpeg libraries";
        return false;
    }

    m_paths = paths;
    m_ff = std::move(handler);
    return true;
}

Ret VideoTranscoder::transcodeWithLeadIn(const io::path_t& videoSource, const io::path_t& destination, const Options& options)
{
    if (!m_ff) {
        return make_ret(Ret::Code::InternalError, std::string("FFmpeg isn't loaded"));
    }

    FFmpegLibHandler& ff = *m_ff;

    auto error = [](const std::string& text) {
        LOGE() << text;
        return make_ret(Ret::Code::UnknownError, text);
    };

    VideoDecoder decoder;
    if (!decoder.load(m_paths) || !decoder.open(videoSource)) {
        return error("unable to read the video " + videoSource.toStdString());
    }

    const VideoStreamInfo info = decoder.streamInfo();
    const int width = info.width;
    const int height = info.height;
    const double fps = info.frameRate > 0.0 ? info.frameRate : 25.0;
    const int64_t bitRate = options.bitRate > 0 ? options.bitRate : (info.bitRate > 0 ? info.bitRate : 8000000);

    const AVCodec* encoder = ff.avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        return error("no H.264 encoder");
    }

    AVFormatContext* out = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    DEFER {
        if (packet) {
            ff.av_packet_free(&packet);
        }
        if (frame) {
            ff.av_frame_free(&frame);
        }
        if (codec) {
            ff.avcodec_free_context(&codec);
        }
        if (out) {
            if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
                ff.avio_close(out->pb);
            }
            ff.avformat_free_context(out);
        }
    };

    if (ff.avformat_alloc_output_context2(&out, nullptr, nullptr, destination.c_str()) < 0 || !out) {
        return error("unsupported output format for " + destination.toStdString());
    }

    codec = ff.avcodec_alloc_context3(encoder);
    if (!codec) {
        return error("out of memory");
    }

    //! NOTE Same size and frame rate as the source, its bit rate; a time base of one frame
    const int fpsMillis = static_cast<int>(std::llround(fps * 1000.0));
    codec->width = width;
    codec->height = height;
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational { 1000, fpsMillis };
    codec->framerate = AVRational { fpsMillis, 1000 };
    codec->bit_rate = bitRate;
    codec->gop_size = static_cast<int>(std::llround(fps * 2.0));
    codec->max_b_frames = 0;
    if (out->oformat->flags & AVFMT_GLOBALHEADER) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (ff.avcodec_open2(codec, encoder, nullptr) < 0) {
        return error(std::string("unable to open the video encoder ") + encoder->name);
    }

    AVStream* stream = ff.avformat_new_stream(out, nullptr);
    if (!stream || ff.avcodec_parameters_from_context(stream->codecpar, codec) < 0) {
        return error("unable to create the output stream");
    }
    stream->time_base = codec->time_base;

    if (!(out->oformat->flags & AVFMT_NOFILE) && ff.avio_open(&out->pb, destination.c_str(), AVIO_FLAG_WRITE) < 0) {
        return error("unable to create " + destination.toStdString());
    }

    if (ff.avformat_write_header(out, nullptr) < 0) {
        return error("unable to write the header of " + destination.toStdString());
    }

    frame = ff.av_frame_alloc();
    packet = ff.av_packet_alloc();
    if (!frame || !packet) {
        return error("out of memory");
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;

    auto drain = [&]() -> bool {
        while (true) {
            const int ret = ff.avcodec_receive_packet(codec, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }

            ff.av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (ff.av_interleaved_write_frame(out, packet) < 0) {
                return false;
            }
        }
    };

    //! NOTE Not reference counted: the encoder copies what it keeps
    auto encode = [&](uint8_t* const planes[3], const int strides[3], int64_t pts) -> bool {
        for (int p = 0; p < 3; ++p) {
            frame->data[p] = planes[p];
            frame->linesize[p] = strides[p];
        }
        frame->pts = pts;
        return ff.avcodec_send_frame(codec, frame) >= 0 && drain();
    };

    const double totalSecs = options.durationSecs > 0.0
                             ? options.durationSecs
                             : options.leadInSecs + std::max(0.0, info.durationSecs);

    auto reportProgress = [&](double secs) {
        if (options.onProgress && totalSecs > 0.0) {
            options.onProgress(std::clamp(secs / totalSecs, 0.0, 1.0));
        }
    };

    // Black lead-in (video range: Y = 16, Cb = Cr = 128)
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    std::vector<uint8_t> black(static_cast<size_t>(width) * height + 2 * static_cast<size_t>(chromaWidth) * chromaHeight, 128);
    std::fill(black.begin(), black.begin() + static_cast<size_t>(width) * height, 16);
    uint8_t* const blackPlanes[3] = {
        black.data(),
        black.data() + static_cast<size_t>(width) * height,
        black.data() + static_cast<size_t>(width) * height + static_cast<size_t>(chromaWidth) * chromaHeight
    };
    const int blackStrides[3] = { width, chromaWidth, chromaWidth };

    const int64_t leadInFrames = static_cast<int64_t>(std::llround(std::max(0.0, options.leadInSecs) * fps));
    int64_t lastPts = -1;
    for (int64_t i = 0; i < leadInFrames; ++i) {
        if (!encode(blackPlanes, blackStrides, i)) {
            return error("unable to encode the video");
        }
        lastPts = i;
    }

    // The video, after the lead-in
    while (VideoFramePtr source = decoder.decodeNextFrame()) {
        const double secs = options.leadInSecs + source->ptsSecs;
        if (options.durationSecs > 0.0 && secs >= options.durationSecs) {
            break;
        }

        int64_t pts = static_cast<int64_t>(std::llround(secs * fps));
        if (pts <= lastPts) {
            pts = lastPts + 1; // timestamps must increase
        }
        lastPts = pts;

        uint8_t* const planes[3] = {
            const_cast<uint8_t*>(source->data.data() + source->offsets[0]),
            const_cast<uint8_t*>(source->data.data() + source->offsets[1]),
            const_cast<uint8_t*>(source->data.data() + source->offsets[2])
        };

        if (!encode(planes, source->strides, pts)) {
            return error("unable to encode the video");
        }

        reportProgress(secs);
    }

    // Flush the encoder
    if (ff.avcodec_send_frame(codec, nullptr) < 0 || !drain()) {
        return error("unable to finish encoding the video");
    }

    if (ff.av_write_trailer(out) < 0) {
        return error("unable to finish " + destination.toStdString());
    }

    reportProgress(totalSecs);

    LOGI() << "transcoded " << videoSource << " with " << options.leadInSecs << " s of black lead-in into " << destination
           << " (" << encoder->name << ", " << width << "x" << height << ", " << fps << " fps, " << bitRate << " b/s)";
    return make_ok();
}
