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

#include <functional>
#include <memory>

#include "global/types/ret.h"
#include "io/path.h"

namespace muse::media {
//! NOTE Re-encodes a video's picture (same size and frame rate, H.264, no audio), preceded by a lead-in of
//! black frames. For when real black frames are needed before a video: a copied (not re-encoded) stream
//! can't be extended, all its frames depending on its own encoding parameters.
class IVideoTranscoder
{
public:
    virtual ~IVideoTranscoder() = default;

    struct Options {
        double leadInSecs = 0.0;   // black frames before the video
        double durationSecs = 0.0; // of the output (lead-in included), 0: until the video ends
        int64_t bitRate = 0;       // 0: the source video's
        std::function<void(double progress)> onProgress; // 0..1
    };

    virtual Ret transcodeWithLeadIn(const io::path_t& videoSource, const io::path_t& destination, const Options& options) = 0;
};

using IVideoTranscoderPtr = std::shared_ptr<IVideoTranscoder>;
}
