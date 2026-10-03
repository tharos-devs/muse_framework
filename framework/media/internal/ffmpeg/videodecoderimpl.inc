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

//! NOTE Included inside each muse::media::ffmpeg::vN namespace (see vN/videodecoder.cpp),
//! after that version's FFmpeg headers and FFmpegLibHandler

struct VideoDecoder::Impl {
    std::shared_ptr<FFmpegLibHandler> ff;

    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;

    VideoStreamInfo info;
    double startSecs = 0.0;
    double frameDurationSecs = 1.0 / 24.0;
    bool demuxEnded = false;

    //! NOTE Frames already decoded by seek(), returned first by decodeNextFrame()
    std::deque<VideoFramePtr> queue;

    //! NOTE decodeThumbnail()'s own position: the last frame shown at or before the previous target,
    //! and the frame decoded after it (if any). Swapped with `frame` rather than copied.
    SwsContext* thumbSws = nullptr;
    AVFrame* thumbPrev = nullptr;
    AVFrame* thumbNext = nullptr;
    bool thumbPositioned = false;
    bool thumbPrevValid = false;
    bool thumbNextValid = false;
    double thumbPrevPts = 0.0;
    double thumbNextPts = 0.0;

    double framePtsSecs(const AVFrame* f) const
    {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = f->pts;
        }
        if (ts == AV_NOPTS_VALUE) {
            return 0.0;
        }
        return ts * av_q2d(stream->time_base) - startSecs;
    }

    //! NOTE Decodes into `frame`; false at the end of the stream or on error
    bool receiveFrame()
    {
        while (true) {
            const int ret = ff->avcodec_receive_frame(codec, frame);
            if (ret == 0) {
                return true;
            }

            if (ret != AVERROR(EAGAIN)) {
                return false; // AVERROR_EOF (fully drained) or a real error
            }

            if (demuxEnded) {
                return false;
            }

            if (ff->av_read_frame(format, packet) < 0) {
                // End of file: enter draining mode, to get the frames still buffered in the decoder
                demuxEnded = true;
                ff->avcodec_send_packet(codec, nullptr);
                continue;
            }

            if (packet->stream_index == streamIndex) {
                ff->avcodec_send_packet(codec, packet);
            }

            ff->av_packet_unref(packet);
        }
    }

    VideoFramePtr convertFrame(double ptsSecs)
    {
        const int width = frame->width;
        const int height = frame->height;
        if (width <= 0 || height <= 0) {
            return nullptr;
        }

        auto result = std::make_shared<VideoFrame>();
        result->ptsSecs = ptsSecs;
        result->width = width;
        result->height = height;

        switch (frame->colorspace) {
        case AVCOL_SPC_BT709: result->colorSpace = VideoFrame::ColorSpace::BT709;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: result->colorSpace = VideoFrame::ColorSpace::BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: result->colorSpace = VideoFrame::ColorSpace::BT2020;
            break;
        default: result->colorSpace = VideoFrame::ColorSpace::Unknown;
            break;
        }

        const int chromaWidth = (width + 1) / 2;
        const int chromaHeight = (height + 1) / 2;
        result->strides[0] = width;
        result->strides[1] = chromaWidth;
        result->strides[2] = chromaWidth;
        result->offsets[0] = 0;
        result->offsets[1] = static_cast<size_t>(width) * height;
        result->offsets[2] = result->offsets[1] + static_cast<size_t>(chromaWidth) * chromaHeight;
        result->data.resize(result->offsets[2] + static_cast<size_t>(chromaWidth) * chromaHeight);

        uint8_t* dst[4] = {
            result->data.data() + result->offsets[0],
            result->data.data() + result->offsets[1],
            result->data.data() + result->offsets[2],
            nullptr
        };
        int dstStrides[4] = { result->strides[0], result->strides[1], result->strides[2], 0 };

        const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(frame->format);
        if (srcFormat == AV_PIX_FMT_YUV420P || srcFormat == AV_PIX_FMT_YUVJ420P) {
            //! NOTE Copied as is, so the range is the source's (a conversion by sws below outputs video range)
            result->fullRange = srcFormat == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;

            // Already the right layout: plain copy of each plane
            const int planeHeights[3] = { height, chromaHeight, chromaHeight };
            const int planeWidths[3] = { width, chromaWidth, chromaWidth };
            for (int p = 0; p < 3; ++p) {
                for (int y = 0; y < planeHeights[p]; ++y) {
                    std::memcpy(dst[p] + static_cast<size_t>(y) * dstStrides[p],
                                frame->data[p] + static_cast<ptrdiff_t>(y) * frame->linesize[p],
                                static_cast<size_t>(planeWidths[p]));
                }
            }
            return result;
        }

        sws = ff->sws_getCachedContext(sws, width, height, srcFormat, width, height, AV_PIX_FMT_YUV420P,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            return nullptr;
        }

        ff->sws_scale(sws, frame->data, frame->linesize, 0, height, dst, dstStrides);
        return result;
    }

    VideoThumbnailPtr convertThumbnail(AVFrame* src, double ptsSecs, int maxWidth, int maxHeight)
    {
        if (src->width <= 0 || src->height <= 0 || maxWidth <= 0 || maxHeight <= 0) {
            return nullptr;
        }

        // Fit in maxWidth x maxHeight, keeping the aspect ratio (of the pixels too)
        double aspect = static_cast<double>(src->width) / src->height;
        if (src->sample_aspect_ratio.num > 0 && src->sample_aspect_ratio.den > 0) {
            aspect *= av_q2d(src->sample_aspect_ratio);
        }

        int width = maxWidth;
        int height = static_cast<int>(std::lround(maxWidth / aspect));
        if (height > maxHeight) {
            height = maxHeight;
            width = static_cast<int>(std::lround(maxHeight * aspect));
        }
        width = std::max(1, width);
        height = std::max(1, height);

        thumbSws = ff->sws_getCachedContext(thumbSws, src->width, src->height, static_cast<AVPixelFormat>(src->format),
                                            width, height, AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr);
        if (!thumbSws) {
            return nullptr;
        }

        auto result = std::make_shared<VideoThumbnail>();
        result->ptsSecs = ptsSecs;
        result->width = width;
        result->height = height;
        result->rgba.resize(static_cast<size_t>(width) * height * 4);

        uint8_t* dst[4] = { result->rgba.data(), nullptr, nullptr, nullptr };
        int dstStrides[4] = { width * 4, 0, 0, 0 };
        ff->sws_scale(thumbSws, src->data, src->linesize, 0, src->height, dst, dstStrides);

        return result;
    }

    void closeAll()
    {
        if (!ff) {
            return;
        }

        if (sws) {
            ff->sws_freeContext(sws);
            sws = nullptr;
        }
        if (thumbSws) {
            ff->sws_freeContext(thumbSws);
            thumbSws = nullptr;
        }
        if (thumbPrev) {
            ff->av_frame_free(&thumbPrev);
        }
        if (thumbNext) {
            ff->av_frame_free(&thumbNext);
        }
        thumbPositioned = false;
        thumbPrevValid = false;
        thumbNextValid = false;
        if (frame) {
            ff->av_frame_free(&frame);
        }
        if (packet) {
            ff->av_packet_free(&packet);
        }
        if (codec) {
            ff->avcodec_free_context(&codec);
        }
        if (format) {
            ff->avformat_close_input(&format);
        }

        stream = nullptr;
        streamIndex = -1;
        info = VideoStreamInfo();
        startSecs = 0.0;
        demuxEnded = false;
        queue.clear();
    }
};

VideoDecoder::VideoDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

VideoDecoder::~VideoDecoder()
{
    close();
}

bool VideoDecoder::load(const FFmpegLibPaths& paths)
{
    auto handler = std::make_shared<FFmpegLibHandler>();
    if (!handler->loadLib(paths.avUtilPath, paths.avCodecPath, paths.avFormatPath, paths.swScalePath, paths.swResamplePath)
        || !handler->loadApi()) {
        LOGW() << "FFmpeg libraries not found";
        return false;
    }

    m_impl->ff = std::move(handler);
    return true;
}

bool VideoDecoder::open(const io::path_t& filePath)
{
    close();

    Impl& d = *m_impl;
    if (!d.ff) {
        return false;
    }

    if (d.ff->avformat_open_input(&d.format, filePath.c_str(), nullptr, nullptr) < 0) {
        LOGE() << "unable to open " << filePath;
        return false;
    }

    if (d.ff->avformat_find_stream_info(d.format, nullptr) < 0) {
        LOGE() << "unable to read stream info of " << filePath;
        d.closeAll();
        return false;
    }

    const AVCodec* decoder = nullptr;
    d.streamIndex = d.ff->av_find_best_stream(d.format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (d.streamIndex < 0 || !decoder) {
        LOGE() << "no decodable video stream in " << filePath;
        d.closeAll();
        return false;
    }

    d.stream = d.format->streams[d.streamIndex];

    d.codec = d.ff->avcodec_alloc_context3(decoder);
    if (!d.codec || d.ff->avcodec_parameters_to_context(d.codec, d.stream->codecpar) < 0) {
        d.closeAll();
        return false;
    }

    d.codec->thread_count = 0; // auto

    if (d.ff->avcodec_open2(d.codec, decoder, nullptr) < 0) {
        LOGE() << "unable to open the video decoder for " << filePath;
        d.closeAll();
        return false;
    }

    d.packet = d.ff->av_packet_alloc();
    d.frame = d.ff->av_frame_alloc();
    if (!d.packet || !d.frame) {
        d.closeAll();
        return false;
    }

    const double timeBase = av_q2d(d.stream->time_base);
    d.startSecs = d.stream->start_time != AV_NOPTS_VALUE ? d.stream->start_time * timeBase : 0.0;

    d.info.width = d.stream->codecpar->width;
    d.info.height = d.stream->codecpar->height;
    d.info.codecName = d.ff->avcodec_get_name(d.stream->codecpar->codec_id);
    d.info.bitRate = d.stream->codecpar->bit_rate > 0 ? d.stream->codecpar->bit_rate : d.format->bit_rate;

    if (d.stream->duration != AV_NOPTS_VALUE && d.stream->duration > 0) {
        d.info.durationSecs = d.stream->duration * timeBase;
    } else if (d.format->duration != AV_NOPTS_VALUE && d.format->duration > 0) {
        d.info.durationSecs = static_cast<double>(d.format->duration) / AV_TIME_BASE;
    }

    AVRational rate = d.stream->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        rate = d.stream->r_frame_rate;
    }
    d.info.frameRate = (rate.num > 0 && rate.den > 0) ? av_q2d(rate) : 0.0;
    d.frameDurationSecs = d.info.frameRate > 0.0 ? 1.0 / d.info.frameRate : 1.0 / 24.0;

    LOGI() << "video opened: " << filePath << ", " << d.info.width << "x" << d.info.height << ", " << d.info.codecName
           << ", " << d.info.frameRate << " fps, " << d.info.durationSecs << " s";

    return true;
}

void VideoDecoder::close()
{
    m_impl->closeAll();
}

const VideoStreamInfo& VideoDecoder::streamInfo() const
{
    return m_impl->info;
}

bool VideoDecoder::seek(double secs)
{
    Impl& d = *m_impl;
    if (!d.format || !d.codec) {
        return false;
    }

    d.queue.clear();
    d.thumbPositioned = false;

    secs = std::max(0.0, secs);
    const int64_t target = static_cast<int64_t>(std::floor((secs + d.startSecs) / av_q2d(d.stream->time_base)));

    //! NOTE Lands on the keyframe at or before the target; the frames up to the target are then decoded
    //! (converting only those close to it, the others are just skipped)
    if (d.ff->av_seek_frame(d.format, d.streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
        LOGW() << "seek failed at " << secs << " s";
        return false;
    }

    d.ff->avcodec_flush_buffers(d.codec);
    d.demuxEnded = false;

    // A little slack for rounding in the timestamps
    const double epsilon = 0.001;
    VideoFramePtr lastBefore;

    while (d.receiveFrame()) {
        const double pts = d.framePtsSecs(d.frame);

        if (pts <= secs + epsilon) {
            //! NOTE Only the frames that may be the one shown at the target are converted (the others are
            //! just skipped), including the last frames of the stream, shown for any target past its end
            const bool nearTarget = pts >= secs - 1.5 * d.frameDurationSecs;
            const bool nearEnd = d.info.durationSecs > 0.0 && pts >= d.info.durationSecs - 2.0 * d.frameDurationSecs;
            if (nearTarget || nearEnd) {
                if (VideoFramePtr converted = d.convertFrame(pts)) {
                    lastBefore = converted;
                }
            }
            d.ff->av_frame_unref(d.frame);
            continue;
        }

        // First frame after the target: the one before it is the one shown at the target
        VideoFramePtr after = d.convertFrame(pts);
        d.ff->av_frame_unref(d.frame);

        if (lastBefore) {
            d.queue.push_back(lastBefore);
        }
        if (after) {
            d.queue.push_back(after);
        }
        return true;
    }

    // Target at (or past) the very end: show the last frame
    if (lastBefore) {
        d.queue.push_back(lastBefore);
    }

    return true;
}

VideoFramePtr VideoDecoder::decodeNextFrame()
{
    Impl& d = *m_impl;

    if (!d.queue.empty()) {
        VideoFramePtr next = d.queue.front();
        d.queue.pop_front();
        return next;
    }

    if (!d.format || !d.codec) {
        return nullptr;
    }

    d.thumbPositioned = false; // decodeThumbnail()'s position is lost: it seeks again next time

    //! NOTE A frame that can't be converted is skipped, rather than ending the stream
    while (d.receiveFrame()) {
        VideoFramePtr result = d.convertFrame(d.framePtsSecs(d.frame));
        d.ff->av_frame_unref(d.frame);
        if (result) {
            return result;
        }
    }

    return nullptr;
}

VideoThumbnailPtr VideoDecoder::decodeThumbnail(double secs, double toleranceSecs, int maxWidth, int maxHeight)
{
    Impl& d = *m_impl;
    if (!d.format || !d.codec) {
        return nullptr;
    }

    if (!d.thumbPrev) {
        d.thumbPrev = d.ff->av_frame_alloc();
        d.thumbNext = d.ff->av_frame_alloc();
        if (!d.thumbPrev || !d.thumbNext) {
            return nullptr;
        }

        //! NOTE Skipping the deblocking filter makes decoding noticeably faster; its artifacts don't
        //! show once scaled down
        d.codec->skip_loop_filter = AVDISCARD_ALL;
    }

    // A little slack for rounding in the timestamps
    const double epsilon = 0.001;
    //! NOTE Decoding forward up to this far beats seeking back to a keyframe and decoding from there
    const double forwardReachSecs = 2.0;

    secs = std::max(0.0, secs);

    const bool canDecodeForward = d.thumbPositioned && d.thumbPrevValid
                                  && secs >= d.thumbPrevPts - epsilon && secs - d.thumbPrevPts <= forwardReachSecs;

    bool justSeeked = false;
    if (!canDecodeForward) {
        const int64_t target = static_cast<int64_t>(std::floor((secs + d.startSecs) / av_q2d(d.stream->time_base)));
        if (d.ff->av_seek_frame(d.format, d.streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
            LOGW() << "thumbnail seek failed at " << secs << " s";
            return nullptr;
        }

        d.ff->avcodec_flush_buffers(d.codec);
        d.demuxEnded = false;
        d.queue.clear(); // decodeNextFrame()'s position is lost anyway
        d.thumbPositioned = true;
        d.thumbPrevValid = false;
        d.thumbNextValid = false;
        justSeeked = true;
    }

    while (true) {
        if (!d.thumbNextValid) {
            if (!d.receiveFrame()) {
                break; // end of the stream: the last frame is shown
            }
            std::swap(d.frame, d.thumbNext);
            d.thumbNextPts = d.framePtsSecs(d.thumbNext);
            d.thumbNextValid = true;
        }

        if (d.thumbNextPts > secs + epsilon && d.thumbPrevValid) {
            break; // thumbPrev is the frame shown at secs
        }

        std::swap(d.thumbPrev, d.thumbNext);
        d.thumbPrevPts = d.thumbNextPts;
        d.thumbPrevValid = true;
        d.thumbNextValid = false;

        //! NOTE Right after a seek, thumbPrev is the keyframe at or before secs: good enough if close
        if (justSeeked && secs - d.thumbPrevPts <= toleranceSecs) {
            break;
        }
        justSeeked = false;
    }

    if (!d.thumbPrevValid) {
        return nullptr;
    }

    return d.convertThumbnail(d.thumbPrev, d.thumbPrevPts, maxWidth, maxHeight);
}
