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

//! NOTE Included inside each muse::media::ffmpeg::vN namespace (see vN/videoremuxer.h), like the decoder

class FFmpegLibHandler;

class VideoRemuxer : public IVideoRemuxer
{
public:
    bool load(const FFmpegLibPaths& paths);

    Ret remux(const io::path_t& videoSource, const io::path_t& audioSource, const io::path_t& destination,
              const Options& options) override;

private:
    std::shared_ptr<FFmpegLibHandler> m_ff;
};
