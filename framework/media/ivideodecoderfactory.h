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

#include "io/path.h"
#include "modularity/imoduleinterface.h"

#include "ivideodecoder.h"

namespace muse::media {
class IVideoDecoderFactory : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IVideoDecoderFactory)

public:
    virtual ~IVideoDecoderFactory() = default;

    //! NOTE Loads the first complete set of supported FFmpeg libraries (v4 to v8) found in the given
    //! directories, in order, independently of the FFmpeg configured for video export. nullptr if none.
    virtual IVideoDecoderPtr createDecoder(const io::paths_t& ffmpegLibsDirs) const = 0;
};
}
