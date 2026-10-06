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

namespace {
//! NOTE Encodes the PCM of an input (e.g. a WAV file) with a lossless codec, on demand: next() gives the next
//! encoded packet, its timestamps in timeBase() from the input's first sample
class LosslessAudioEncoder
{
public:
    LosslessAudioEncoder(FFmpegLibHandler& ff, AVFormatContext* in, AVStream* inStream)
        : m_ff(ff), m_in(in), m_inStream(inStream)
    {
    }

    ~LosslessAudioEncoder()
    {
        if (m_frame) {
            m_ff.av_frame_free(&m_frame);
        }
        if (m_ctx) {
            m_ff.avcodec_free_context(&m_ctx);
        }
    }

    std::string open(IVideoRemuxer::Options::AudioEncoding encoding, int bitsPerSample, AVFormatContext* out, AVStream* outStream)
    {
        const AVCodecParameters* par = m_inStream->codecpar;

        switch (par->codec_id) {
        case AV_CODEC_ID_PCM_S16LE: m_inBytes = 2;
            break;
        case AV_CODEC_ID_PCM_S24LE: m_inBytes = 3;
            break;
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_F32LE: m_inBytes = 4;
            break;
        default:
            return "the audio to encode isn't PCM";
        }
        m_inFloat = par->codec_id == AV_CODEC_ID_PCM_F32LE;

#if LIBAVUTIL_VERSION_MAJOR >= 58
        m_channels = par->ch_layout.nb_channels;
#else
        m_channels = par->channels;
#endif
        if (m_channels < 1 || m_channels > 2 || par->sample_rate <= 0) {
            return "unsupported audio channels or sample rate";
        }

        m_bits = bitsPerSample == 16 ? 16 : 24;

        const bool alac = encoding == IVideoRemuxer::Options::AudioEncoding::Alac;
        const AVCodec* codec = m_ff.avcodec_find_encoder(alac ? AV_CODEC_ID_ALAC : AV_CODEC_ID_FLAC);
        if (!codec) {
            return std::string("no ") + (alac ? "ALAC" : "FLAC") + " encoder";
        }

        m_ctx = m_ff.avcodec_alloc_context3(codec);
        m_frame = m_ff.av_frame_alloc();
        if (!m_ctx || !m_frame) {
            return "out of memory";
        }

        //! NOTE Both take their samples in the high bits of 32 bits integers above 16 bits
        if (alac) {
            m_ctx->sample_fmt = m_bits == 16 ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S32P;
        } else {
            m_ctx->sample_fmt = m_bits == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
        }
        m_planar = alac;

        m_ctx->sample_rate = par->sample_rate;
        m_ctx->bits_per_raw_sample = m_bits;
        m_ctx->time_base = AVRational { 1, par->sample_rate };
        setLayout(m_ctx);
        if (out->oformat->flags & AVFMT_GLOBALHEADER) {
            m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (m_ff.avcodec_open2(m_ctx, codec, nullptr) < 0) {
            return "unable to open the audio encoder";
        }

        if (m_ff.avcodec_parameters_from_context(outStream->codecpar, m_ctx) < 0) {
            return "unable to set the audio stream";
        }

        return std::string();
    }

    AVRational timeBase() const
    {
        return m_ctx->time_base;
    }

    bool failed() const
    {
        return m_failed;
    }

    //! NOTE False at the end, or on failure
    bool next(AVPacket* packet)
    {
        while (true) {
            const int ret = m_ff.avcodec_receive_packet(m_ctx, packet);
            if (ret == 0) {
                return true;
            }
            if (ret != AVERROR(EAGAIN)) {
                m_failed = ret != AVERROR_EOF;
                return false;
            }
            if (!sendFrame()) {
                return false;
            }
        }
    }

private:
    void setLayout(AVCodecContext* ctx) const
    {
#if LIBAVUTIL_VERSION_MAJOR >= 58
        ctx->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
        ctx->ch_layout.nb_channels = m_channels;
        ctx->ch_layout.u.mask = m_channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#else
        ctx->channels = m_channels;
        ctx->channel_layout = m_channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
    }

    void setLayout(AVFrame* frame) const
    {
#if LIBAVUTIL_VERSION_MAJOR >= 58
        frame->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
        frame->ch_layout.nb_channels = m_channels;
        frame->ch_layout.u.mask = m_channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#else
        frame->channels = m_channels;
        frame->channel_layout = m_channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
    }

    //! NOTE Uniform in [0, 1) (xorshift64: fast, and the same output for the same input)
    double nextRandom()
    {
        m_random ^= m_random << 13;
        m_random ^= m_random >> 7;
        m_random ^= m_random << 17;
        return static_cast<double>(m_random >> 11) / static_cast<double>(1ULL << 53);
    }

    size_t pendingFrames() const
    {
        return (m_pending.size() - m_pendingStart) / static_cast<size_t>(m_channels);
    }

    //! NOTE Reads the next input packet into m_pending (samples at m_bits bits), false at the end
    bool readInput(AVPacket* packet)
    {
        while (m_ff.av_read_frame(m_in, packet) >= 0) {
            if (packet->stream_index != m_inStream->index) {
                m_ff.av_packet_unref(packet);
                continue;
            }

            const int count = packet->size / m_inBytes;
            const uint8_t* data = packet->data;
            const int32_t maxValue = (1 << (m_bits - 1)) - 1;
            const int32_t minValue = -(1 << (m_bits - 1));

            for (int i = 0; i < count; ++i, data += m_inBytes) {
                int32_t value = 0;
                if (m_inFloat) {
                    float sample = 0.f;
                    std::memcpy(&sample, data, sizeof(sample));
                    //! NOTE Down to 16 bits, a TPDF dither (±1 LSB) turns the rounding's distortion of quiet passages
                    //! into a constant, inaudible noise floor
                    const double dither = m_bits == 16 ? nextRandom() - nextRandom() : 0.0;
                    const double scaled = std::round(static_cast<double>(sample) * (1 << (m_bits - 1)) + dither);
                    value = static_cast<int32_t>(std::clamp(scaled, static_cast<double>(minValue), static_cast<double>(maxValue)));
                } else {
                    // Little endian, sign extended from the top byte
                    uint32_t raw = 0;
                    for (int b = 0; b < m_inBytes; ++b) {
                        raw |= static_cast<uint32_t>(data[b]) << (8 * (b + 4 - m_inBytes));
                    }
                    const int32_t full = static_cast<int32_t>(raw); // at 32 bits
                    value = full >> (32 - m_bits);
                }
                m_pending.push_back(value);
            }

            m_ff.av_packet_unref(packet);
            return true;
        }

        return false;
    }

    bool sendFrame()
    {
        const size_t frameSize = m_ctx->frame_size > 0 ? static_cast<size_t>(m_ctx->frame_size) : 4096;

        AVPacket* inPacket = m_ff.av_packet_alloc();
        while (!m_inputEnded && pendingFrames() < frameSize) {
            if (!readInput(inPacket)) {
                m_inputEnded = true;
            }
        }
        m_ff.av_packet_free(&inPacket);

        const size_t count = std::min(frameSize, pendingFrames());
        if (count == 0) {
            if (m_flushed) {
                return false;
            }
            m_flushed = true;
            if (m_ff.avcodec_send_frame(m_ctx, nullptr) < 0) {
                m_failed = true;
                return false;
            }
            return true;
        }

        const int bytes = m_bits == 16 ? 2 : 4;
        const int shift = m_bits == 16 ? 0 : 32 - m_bits;
        const size_t planes = m_planar ? static_cast<size_t>(m_channels) : 1;
        const size_t planeSamples = m_planar ? count : count * m_channels;

        m_planes.resize(planes);
        for (size_t p = 0; p < planes; ++p) {
            m_planes[p].resize(planeSamples * bytes);
        }

        const int32_t* src = m_pending.data() + m_pendingStart;
        for (size_t i = 0; i < count; ++i) {
            for (int ch = 0; ch < m_channels; ++ch) {
                const int32_t value = src[i * m_channels + ch];
                const size_t plane = m_planar ? static_cast<size_t>(ch) : 0;
                const size_t index = m_planar ? i : i * m_channels + ch;
                if (bytes == 2) {
                    const int16_t v = static_cast<int16_t>(value);
                    std::memcpy(m_planes[plane].data() + index * 2, &v, 2);
                } else {
                    const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(value) << shift);
                    std::memcpy(m_planes[plane].data() + index * 4, &v, 4);
                }
            }
        }

        m_pendingStart += count * m_channels;
        if (m_pendingStart > m_pending.size() / 2) {
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingStart));
            m_pendingStart = 0;
        }

        //! NOTE A frame without buffers of its own: the encoder copies it
        m_ff.av_frame_unref(m_frame);
        m_frame->format = m_ctx->sample_fmt;
        m_frame->sample_rate = m_ctx->sample_rate;
        m_frame->nb_samples = static_cast<int>(count);
        setLayout(m_frame);
        for (size_t p = 0; p < planes; ++p) {
            m_frame->data[p] = m_planes[p].data();
            m_frame->linesize[p] = static_cast<int>(m_planes[p].size());
        }
        m_frame->extended_data = m_frame->data;
        m_frame->pts = m_samplesSent;
        m_samplesSent += static_cast<int64_t>(count);

        if (m_ff.avcodec_send_frame(m_ctx, m_frame) < 0) {
            m_failed = true;
            return false;
        }

        return true;
    }

    FFmpegLibHandler& m_ff;
    AVFormatContext* m_in = nullptr;
    AVStream* m_inStream = nullptr;
    AVCodecContext* m_ctx = nullptr;
    AVFrame* m_frame = nullptr;

    int m_inBytes = 0;
    bool m_inFloat = false;
    int m_channels = 0;
    int m_bits = 24;
    bool m_planar = false;

    std::vector<int32_t> m_pending; // interleaved, at m_bits bits
    size_t m_pendingStart = 0;
    std::vector<std::vector<uint8_t> > m_planes;
    int64_t m_samplesSent = 0;
    bool m_inputEnded = false;
    bool m_flushed = false;
    bool m_failed = false;
    uint64_t m_random = 0x9E3779B97F4A7C15ULL;
};
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
        || ff.avcodec_parameters_copy(videoOutStream->codecpar, videoInStream->codecpar) < 0) {
        return error("unable to create the output streams");
    }

    std::unique_ptr<LosslessAudioEncoder> audioEncoder;
    if (options.audioEncoding == Options::AudioEncoding::Copy) {
        if (ff.avcodec_parameters_copy(audioOutStream->codecpar, audioInStream->codecpar) < 0) {
            return error("unable to create the output streams");
        }
    } else {
        audioEncoder = std::make_unique<LosslessAudioEncoder>(ff, audioIn, audioInStream);
        const std::string encoderError = audioEncoder->open(options.audioEncoding, options.audioBitsPerSample, out, audioOutStream);
        if (!encoderError.empty()) {
            return error(encoderError);
        }

        //! NOTE FLAC in MP4 is still flagged experimental by some FFmpeg versions' muxer, though standardized
        if (options.audioEncoding == Options::AudioEncoding::Flac) {
            out->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        }
    }

    // Let the muxer pick the tags valid for its container
    videoOutStream->codecpar->codec_tag = 0;
    audioOutStream->codecpar->codec_tag = 0;

    //! NOTE ...except HEVC: the muxer's default, "hev1", isn't played by Apple's players (QuickTime, Safari, iOS),
    //! "hvc1" (parameter sets in the sample description, as they are in an MP4/MOV source) is
    if (videoOutStream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        videoOutStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }
    videoOutStream->time_base = videoInStream->time_base;
    audioOutStream->time_base = audioEncoder ? audioEncoder->timeBase() : audioInStream->time_base;

    if (!(out->oformat->flags & AVFMT_NOFILE) && ff.avio_open(&out->pb, destination.c_str(), AVIO_FLAG_WRITE) < 0) {
        return error("unable to create " + destination.toStdString());
    }

    //! NOTE The index at the start of the file ("fast start"), better for uploading and streaming
    AVDictionary* muxerOptions = nullptr;
    ff.av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
    const int headerRet = ff.avformat_write_header(out, &muxerOptions);
    ff.av_dict_free(&muxerOptions);

    if (headerRet < 0) {
        //! NOTE Typically a video codec the container doesn't accept
        return error("the video's codec can't be stored in " + destination.toStdString());
    }

    const AVRational videoTb = videoInStream->time_base;
    const AVRational audioTb = audioEncoder ? audioEncoder->timeBase() : audioInStream->time_base;

    auto secsToTs = [](double secs, AVRational tb) {
        return static_cast<int64_t>(std::llround(secs / av_q2d(tb)));
    };

    //! NOTE Output time = input time - shift. The video's time 0 is its first frame, whose timestamp may not be 0
    const int64_t videoFirst = videoInStream->start_time != AV_NOPTS_VALUE ? videoInStream->start_time : 0;
    const int64_t audioFirst = !audioEncoder && audioInStream->start_time != AV_NOPTS_VALUE ? audioInStream->start_time : 0;
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

            //! NOTE Cut in decoding order, not by presentation time: a frame shown after the end can be a reference
            //! of frames shown before it (B-frames), dropping it would corrupt them. At most a few frames past the
            //! end are kept, harmless
            if (hasDuration && lastVideoSecs >= options.durationSecs) {
                ff.av_packet_unref(packet);
                videoEnded = true;
                continue;
            }

            ff.av_packet_rescale_ts(packet, videoTb, videoOutStream->time_base);
            packet->stream_index = videoOutStream->index;
        } else {
            if (audioEncoder ? !audioEncoder->next(packet) : !readNext(audioIn, audioIndex)) {
                if (audioEncoder && audioEncoder->failed()) {
                    return error("unable to encode the audio");
                }
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
