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

#include <QCoreApplication>
#include <QFileInfo>
#include <QLibraryInfo>

#include "io/fileinfo.h"

#include "ffmpegutils.h"

#include "internal/ffmpeg/v8/videodecoder.h"
#include "internal/ffmpeg/v8/videoremuxer.h"
#include "internal/ffmpeg/v8/videotranscoder.h"
#include "internal/ffmpeg/v7/videodecoder.h"
#include "internal/ffmpeg/v7/videoremuxer.h"
#include "internal/ffmpeg/v7/videotranscoder.h"
#include "internal/ffmpeg/v6/videodecoder.h"
#include "internal/ffmpeg/v6/videoremuxer.h"
#include "internal/ffmpeg/v6/videotranscoder.h"
#include "internal/ffmpeg/v5/videodecoder.h"
#include "internal/ffmpeg/v5/videoremuxer.h"
#include "internal/ffmpeg/v5/videotranscoder.h"
#include "internal/ffmpeg/v4/videodecoder.h"
#include "internal/ffmpeg/v4/videoremuxer.h"
#include "internal/ffmpeg/v4/videotranscoder.h"

#include "log.h"

using namespace muse;
using namespace muse::media;

namespace {
template<typename T>
std::shared_ptr<T> tryLoad(const FFmpegLibPaths& paths)
{
    auto object = std::make_shared<T>();
    if (object->load(paths)) {
        return object;
    }
    return nullptr;
}

//! NOTE Calls `make(version, paths)` with the libraries of the first directory really containing a
//! supported FFmpeg, until it returns something
template<typename Ptr, typename Make>
Ptr createFromDirs(const io::paths_t& ffmpegLibsDirs, const Make& make)
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

        if (Ptr result = make(versionFromAVFormatPath(paths.avFormatPath), paths)) {
            LOGI() << "using the FFmpeg libraries in " << dir;
            return result;
        }
    }

    return nullptr;
}
}

io::paths_t VideoDecoderFactory::defaultFFmpegLibsDirs() const
{
    io::paths_t dirs;

    const QString appDir = QCoreApplication::applicationDirPath();

    //! NOTE Where the deployment tools put the FFmpeg libraries of Qt Multimedia's FFmpeg backend:
    //! next to the executable on Windows (windeployqt), in the bundle's Frameworks on macOS (macdeployqt)
#if defined(Q_OS_MAC)
    dirs.push_back(io::path_t(appDir + "/../Frameworks"));
#endif
    dirs.push_back(io::path_t(appDir));

    //! NOTE Development builds use Qt's own installation directly
    dirs.push_back(io::path_t(QLibraryInfo::path(QLibraryInfo::LibrariesPath)));
    dirs.push_back(io::path_t(QLibraryInfo::path(QLibraryInfo::BinariesPath)));

    //! NOTE Last resort: the FFmpeg the user configured for video export
    if (configuration()) {
        dirs.push_back(configuration()->ffmpegLibsDir());
    }

    return dirs;
}

IVideoDecoderPtr VideoDecoderFactory::createDecoder(const io::paths_t& ffmpegLibsDirs) const
{
    return createFromDirs<IVideoDecoderPtr>(ffmpegLibsDirs, [](FFmpegVersion version, const FFmpegLibPaths& paths) -> IVideoDecoderPtr {
        switch (version) {
            case FFMPEG_V8: return tryLoad<ffmpeg::v8::VideoDecoder>(paths);
            case FFMPEG_V7: return tryLoad<ffmpeg::v7::VideoDecoder>(paths);
            case FFMPEG_V6: return tryLoad<ffmpeg::v6::VideoDecoder>(paths);
            case FFMPEG_V5: return tryLoad<ffmpeg::v5::VideoDecoder>(paths);
            case FFMPEG_V4: return tryLoad<ffmpeg::v4::VideoDecoder>(paths);
            default: return nullptr;
        }
    });
}

IVideoRemuxerPtr VideoDecoderFactory::createRemuxer(const io::paths_t& ffmpegLibsDirs) const
{
    return createFromDirs<IVideoRemuxerPtr>(ffmpegLibsDirs, [](FFmpegVersion version, const FFmpegLibPaths& paths) -> IVideoRemuxerPtr {
        switch (version) {
            case FFMPEG_V8: return tryLoad<ffmpeg::v8::VideoRemuxer>(paths);
            case FFMPEG_V7: return tryLoad<ffmpeg::v7::VideoRemuxer>(paths);
            case FFMPEG_V6: return tryLoad<ffmpeg::v6::VideoRemuxer>(paths);
            case FFMPEG_V5: return tryLoad<ffmpeg::v5::VideoRemuxer>(paths);
            case FFMPEG_V4: return tryLoad<ffmpeg::v4::VideoRemuxer>(paths);
            default: return nullptr;
        }
    });
}

IVideoTranscoderPtr VideoDecoderFactory::createTranscoder(const io::paths_t& ffmpegLibsDirs) const
{
    return createFromDirs<IVideoTranscoderPtr>(ffmpegLibsDirs, [](FFmpegVersion version, const FFmpegLibPaths& paths) -> IVideoTranscoderPtr {
        switch (version) {
            case FFMPEG_V8: return tryLoad<ffmpeg::v8::VideoTranscoder>(
                    paths);
            case FFMPEG_V7: return tryLoad<ffmpeg::v7::VideoTranscoder>(paths);
            case FFMPEG_V6: return tryLoad<ffmpeg::v6::VideoTranscoder>(paths);
            case FFMPEG_V5: return tryLoad<ffmpeg::v5::VideoTranscoder>(paths);
            case FFMPEG_V4: return tryLoad<ffmpeg::v4::VideoTranscoder>(paths);
            default: return nullptr;
        }
    });
}
