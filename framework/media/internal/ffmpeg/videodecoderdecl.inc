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

//! NOTE Included inside each muse::media::ffmpeg::vN namespace (see vN/videodecoder.h): the decoder is
//! written once, but compiled against each FFmpeg version's own headers, like the lib handlers

class FFmpegLibHandler;

class VideoDecoder : public IVideoDecoder
{
public:
    VideoDecoder();
    ~VideoDecoder() override;

    bool load(const FFmpegLibPaths& paths);

    bool open(const io::path_t& filePath) override;
    void close() override;

    const VideoStreamInfo& streamInfo() const override;

    bool seek(double secs) override;
    VideoFramePtr decodeNextFrame() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
