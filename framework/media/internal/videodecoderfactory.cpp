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

#include "videodecoderfactory.h"

#include <QFileInfo>

#include "io/fileinfo.h"

#include "ffmpegutils.h"

#include "internal/ffmpeg/v8/videodecoder.h"
#include "internal/ffmpeg/v7/videodecoder.h"
#include "internal/ffmpeg/v6/videodecoder.h"
#include "internal/ffmpeg/v5/videodecoder.h"
#include "internal/ffmpeg/v4/videodecoder.h"

#include "log.h"

using namespace muse::media;

namespace {
template<typename Decoder>
IVideoDecoderPtr tryCreateDecoder(const FFmpegLibPaths& paths)
{
    auto decoder = std::make_shared<Decoder>();
    if (decoder->load(paths)) {
        return decoder;
    }
    return nullptr;
}
}

IVideoDecoderPtr VideoDecoderFactory::createDecoder(const io::paths_t& ffmpegLibsDirs) const
{
    for (const io::path_t& dir : ffmpegLibsDirs) {
        if (dir.empty() || !io::FileInfo::exists(dir)) {
            continue;
        }

        //! NOTE findLibraryPaths() also falls back to system-wide locations: only accept libraries
        //! really found in this directory, so the order of ffmpegLibsDirs is respected
        const FFmpegLibPaths paths = findLibraryPaths(dir);
        if (paths.avFormatPath.empty()
            || QFileInfo(io::dirpath(paths.avFormatPath).toQString()).canonicalFilePath()
            != QFileInfo(dir.toQString()).canonicalFilePath()) {
            continue;
        }

        IVideoDecoderPtr decoder;
        switch (versionFromAVFormatPath(paths.avFormatPath)) {
        case FFMPEG_V8: decoder = tryCreateDecoder<ffmpeg::v8::VideoDecoder>(paths);
            break;
        case FFMPEG_V7: decoder = tryCreateDecoder<ffmpeg::v7::VideoDecoder>(paths);
            break;
        case FFMPEG_V6: decoder = tryCreateDecoder<ffmpeg::v6::VideoDecoder>(paths);
            break;
        case FFMPEG_V5: decoder = tryCreateDecoder<ffmpeg::v5::VideoDecoder>(paths);
            break;
        case FFMPEG_V4: decoder = tryCreateDecoder<ffmpeg::v4::VideoDecoder>(paths);
            break;
        default:
            break;
        }

        if (decoder) {
            LOGI() << "video decoder: using the FFmpeg libraries in " << dir;
            return decoder;
        }
    }

    return nullptr;
}
