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

#include <memory>

#include "global/types/ret.h"
#include "io/path.h"

namespace muse::media {
//! NOTE Combines the picture (best video stream) of a video file with the audio (best audio stream) of another
//! file into a new file, without
//! re-encoding either (the video stream is copied as is, so there's no loss and it's fast). The
//! output container is deduced from the destination's suffix. Positioning is done with timestamps,
//! i.e. edit lists in MP4/MOV: players start exactly at the right frame, or show black meanwhile.
class IVideoRemuxer
{
public:
    virtual ~IVideoRemuxer() = default;

    struct Options {
        //! NOTE The video time shown at the output's start (from the video's first frame). Negative: the
        //! video only starts that much later in the output (black before)
        double videoStartSecs = 0.0;
        //! NOTE The output's duration, 0: until both inputs end
        double durationSecs = 0.0;
        //! NOTE The audio encoder's priming (e.g. 2048 samples for AAC-LC), skipped at playback
        double audioPrimingSecs = 0.0;
    };

    virtual Ret remux(const io::path_t& videoSource, const io::path_t& audioSource, const io::path_t& destination,
                      const Options& options) = 0;
};

using IVideoRemuxerPtr = std::shared_ptr<IVideoRemuxer>;
}
