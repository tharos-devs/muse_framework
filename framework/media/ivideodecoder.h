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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "io/path.h"

namespace muse::media {
struct VideoStreamInfo {
    int width = 0;
    int height = 0;
    double durationSecs = 0.0;
    double frameRate = 0.0; // average, 0 if unknown
    std::string codecName;
    int64_t bitRate = 0;

    bool isValid() const { return width > 0 && height > 0; }
};

//! NOTE A decoded picture, always converted to planar YUV 4:2:0 (8-bit), at the stream's own size
struct VideoFrame {
    double ptsSecs = 0.0; // from the start of the stream (its first frame is at 0)
    int width = 0;
    int height = 0;

    // Y, then U, then V planes, each with its own stride (U/V are half size, rounded up)
    std::vector<uint8_t> data;
    int strides[3] = { 0, 0, 0 };
    size_t offsets[3] = { 0, 0, 0 };
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

//! NOTE Not thread-safe: meant to be driven from a single (worker) thread
class IVideoDecoder
{
public:
    virtual ~IVideoDecoder() = default;

    virtual bool open(const io::path_t& filePath) = 0;
    virtual void close() = 0;

    virtual const VideoStreamInfo& streamInfo() const = 0;

    //! NOTE Frame-accurate: after this, decodeNextFrame() returns the frame shown at `secs`
    //! (the last one whose pts <= secs), then the following ones
    virtual bool seek(double secs) = 0;

    //! NOTE nullptr at the end of the stream or on error
    virtual VideoFramePtr decodeNextFrame() = 0;
};

using IVideoDecoderPtr = std::shared_ptr<IVideoDecoder>;
}
