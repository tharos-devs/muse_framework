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

    //! NOTE The first encoder found for H.264 isn't necessarily usable (e.g. a hardware encoder for a GPU the
    //! machine doesn't have): try them in order of preference, really opening each one, in both pixel formats
    //! encoders commonly take
    static const char* const ENCODERS[] = {
        "libx264", "h264_videotoolbox", "h264_mf", "h264_nvenc", "h264_qsv", "h264_amf", "libopenh264"
    };
    static const AVPixelFormat PIXEL_FORMATS[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12 };

    for (const char* name : ENCODERS) {
        const AVCodec* encoder = handler->avcodec_find_encoder_by_name(name);
        if (!encoder) {
            continue;
        }

        for (AVPixelFormat pixelFormat : PIXEL_FORMATS) {
            AVCodecContext* test = handler->avcodec_alloc_context3(encoder);
            if (!test) {
                continue;
            }

            test->width = 256;
            test->height = 144;
            test->pix_fmt = pixelFormat;
            test->time_base = AVRational { 1, 25 };
            test->framerate = AVRational { 25, 1 };
            test->bit_rate = 1000000;

            const bool opened = handler->avcodec_open2(test, encoder, nullptr) >= 0;
            handler->avcodec_free_context(&test);

            if (opened) {
                m_encoder = encoder;
                m_pixelFormat = static_cast<int>(pixelFormat);
                m_paths = paths;
                m_ff = std::move(handler);
                LOGI() << "H.264 encoder: " << name << (pixelFormat == AV_PIX_FMT_NV12 ? " (NV12)" : " (YUV420P)");
                return true;
            }
        }
    }

    LOGW() << "no usable H.264 encoder in the FFmpeg libraries";
    return false;
}

Ret VideoTranscoder::transcodeWithLeadIn(const io::path_t& videoSource, const io::path_t& destination, const Options& options)
{
    if (!m_ff || !m_encoder) {
        return make_ret(Ret::Code::InternalError, std::string("FFmpeg isn't loaded"));
    }

    FFmpegLibHandler& ff = *m_ff;
    const AVCodec* encoder = static_cast<const AVCodec*>(m_encoder);
    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(m_pixelFormat);

    auto error = [](const std::string& text) {
        LOGE() << text;
        return make_ret(Ret::Code::UnknownError, text);
    };

    auto isCanceled = [&options]() {
        return options.isCanceled && options.isCanceled();
    };

    AVFormatContext* sourceFormat = nullptr;
    AVFormatContext* out = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;

    DEFER {
        if (sws) {
            ff.sws_freeContext(sws);
        }
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
        if (sourceFormat) {
            ff.avformat_close_input(&sourceFormat);
        }
    };

    //! NOTE The source's own stream parameters: exact frame rate, pixel aspect, colors, orientation
    if (ff.avformat_open_input(&sourceFormat, videoSource.c_str(), nullptr, nullptr) < 0
        || ff.avformat_find_stream_info(sourceFormat, nullptr) < 0) {
        return error("unable to read the video " + videoSource.toStdString());
    }

    const int sourceIndex = ff.av_find_best_stream(sourceFormat, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (sourceIndex < 0) {
        return error("no video stream in " + videoSource.toStdString());
    }

    const AVStream* sourceStream = sourceFormat->streams[sourceIndex];
    const AVCodecParameters* sourcePar = sourceStream->codecpar;

    VideoDecoder decoder;
    if (!decoder.load(m_paths) || !decoder.open(videoSource)) {
        return error("unable to decode the video " + videoSource.toStdString());
    }

    const VideoStreamInfo info = decoder.streamInfo();

    //! NOTE 4:2:0 encoders need even dimensions: an odd last column/row is dropped
    const int width = info.width & ~1;
    const int height = info.height & ~1;
    if (width <= 0 || height <= 0) {
        return error("invalid video size");
    }

    AVRational frameRate = sourceStream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = sourceStream->r_frame_rate;
    }
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational { 25, 1 };
    }
    const double fps = av_q2d(frameRate);

    const int64_t bitRate = options.bitRate > 0 ? options.bitRate : (info.bitRate > 0 ? info.bitRate : 8000000);

    if (ff.avformat_alloc_output_context2(&out, nullptr, nullptr, destination.c_str()) < 0 || !out) {
        return error("unsupported output format for " + destination.toStdString());
    }

    codec = ff.avcodec_alloc_context3(encoder);
    if (!codec) {
        return error("out of memory");
    }

    //! NOTE A time base of one frame at the source's exact frame rate
    const bool fullRange = sourcePar->color_range == AVCOL_RANGE_JPEG || sourcePar->format == AV_PIX_FMT_YUVJ420P;
    codec->width = width;
    codec->height = height;
    codec->pix_fmt = pixelFormat;
    codec->time_base = AVRational { frameRate.den, frameRate.num };
    codec->framerate = frameRate;
    codec->bit_rate = bitRate;
    codec->gop_size = static_cast<int>(std::llround(fps * 2.0));
    codec->max_b_frames = 0;
    codec->sample_aspect_ratio = sourcePar->sample_aspect_ratio;
    codec->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    codec->colorspace = sourcePar->color_space;
    codec->color_primaries = sourcePar->color_primaries;
    codec->color_trc = sourcePar->color_trc;
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
    stream->sample_aspect_ratio = sourcePar->sample_aspect_ratio;

#if LIBAVCODEC_VERSION_MAJOR >= 61
    //! NOTE The orientation (display matrix): a portrait phone video would otherwise come out sideways
    if (const AVPacketSideData* matrix = ff.av_packet_side_data_get(sourcePar->coded_side_data, sourcePar->nb_coded_side_data,
                                                                     AV_PKT_DATA_DISPLAYMATRIX)) {
        if (AVPacketSideData* copy = ff.av_packet_side_data_new(&stream->codecpar->coded_side_data,
                                                                &stream->codecpar->nb_coded_side_data,
                                                                AV_PKT_DATA_DISPLAYMATRIX, matrix->size, 0)) {
            std::memcpy(copy->data, matrix->data, matrix->size);
        }
    }
#endif

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

    frame->format = pixelFormat;
    frame->width = width;
    frame->height = height;
    frame->color_range = codec->color_range;
    frame->colorspace = codec->colorspace;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;

    // For encoders taking NV12: converted from our planar frames
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    std::vector<uint8_t> nv12;
    if (pixelFormat == AV_PIX_FMT_NV12) {
        nv12.resize(static_cast<size_t>(width) * height + static_cast<size_t>(chromaWidth) * 2 * chromaHeight);
    }

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
    auto encode = [&](const uint8_t* const planes[3], const int strides[3], int64_t pts) -> bool {
        if (pixelFormat == AV_PIX_FMT_NV12) {
            uint8_t* dst[4] = { nv12.data(), nv12.data() + static_cast<size_t>(width) * height, nullptr, nullptr };
            int dstStrides[4] = { width, chromaWidth * 2, 0, 0 };
            sws = ff.sws_getCachedContext(sws, width, height, AV_PIX_FMT_YUV420P, width, height, AV_PIX_FMT_NV12,
                                          SWS_POINT, nullptr, nullptr, nullptr);
            if (!sws) {
                return false;
            }
            ff.sws_scale(sws, planes, strides, 0, height, dst, dstStrides);

            frame->data[0] = dst[0];
            frame->data[1] = dst[1];
            frame->linesize[0] = dstStrides[0];
            frame->linesize[1] = dstStrides[1];
        } else {
            for (int p = 0; p < 3; ++p) {
                frame->data[p] = const_cast<uint8_t*>(planes[p]);
                frame->linesize[p] = strides[p];
            }
        }

        frame->pts = pts;
        return ff.avcodec_send_frame(codec, frame) >= 0 && drain();
    };

    const double sourceDurationSecs = std::max(0.0, info.durationSecs);
    const double totalSecs = options.durationSecs > 0.0 ? options.durationSecs : std::max(0.0, options.leadInSecs) + sourceDurationSecs;
    const double leadInSecs = std::min(std::max(0.0, options.leadInSecs), totalSecs);

    auto reportProgress = [&](double secs) {
        if (options.onProgress && totalSecs > 0.0) {
            options.onProgress(std::clamp(secs / totalSecs, 0.0, 1.0));
        }
    };

    auto frameIndex = [&](double secs) {
        return static_cast<int64_t>(std::llround(secs * fps));
    };

    // Black lead-in (Y = 0 or 16 depending on the range, Cb = Cr = 128), at most the output's duration
    std::vector<uint8_t> black(static_cast<size_t>(width) * height + 2 * static_cast<size_t>(chromaWidth) * chromaHeight, 128);
    std::fill(black.begin(), black.begin() + static_cast<size_t>(width) * height, fullRange ? 0 : 16);
    const uint8_t* const blackPlanes[3] = {
        black.data(),
        black.data() + static_cast<size_t>(width) * height,
        black.data() + static_cast<size_t>(width) * height + static_cast<size_t>(chromaWidth) * chromaHeight
    };
    const int blackStrides[3] = { width, chromaWidth, chromaWidth };

    const int64_t leadInFrames = frameIndex(leadInSecs);
    int64_t lastPts = -1;
    for (int64_t i = 0; i < leadInFrames; ++i) {
        if (isCanceled()) {
            return make_ret(Ret::Code::Cancel);
        }
        if (!encode(blackPlanes, blackStrides, i)) {
            return error("unable to encode the video");
        }
        lastPts = i;
        reportProgress(i / fps);
    }

    // The video, after the lead-in
    while (VideoFramePtr source = decoder.decodeNextFrame()) {
        if (isCanceled()) {
            return make_ret(Ret::Code::Cancel);
        }

        const double secs = leadInSecs + source->ptsSecs;
        if (options.durationSecs > 0.0 && secs >= options.durationSecs) {
            break;
        }

        //! NOTE A frame of another size (resolution change mid-stream) can't be encoded in this stream
        if (source->width < width || source->height < height) {
            LOGW() << "skipped a frame of another size: " << source->width << "x" << source->height;
            continue;
        }

        //! NOTE Variable frame rate: a frame landing on an already used timestamp is dropped (moving it would
        //! shift the following ones out of sync)
        const int64_t pts = frameIndex(secs);
        if (pts <= lastPts) {
            continue;
        }
        lastPts = pts;

        const uint8_t* const planes[3] = {
            source->data.data() + source->offsets[0],
            source->data.data() + source->offsets[1],
            source->data.data() + source->offsets[2]
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

    LOGI() << "transcoded " << videoSource << " with " << leadInSecs << " s of black lead-in into " << destination
           << " (" << encoder->name << ", " << width << "x" << height << ", " << fps << " fps, " << bitRate << " b/s)";
    return make_ok();
}
