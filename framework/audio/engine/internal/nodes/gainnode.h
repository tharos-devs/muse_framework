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

#include "audionode.h"

namespace muse::audio {
struct GainTag
{
    static constexpr const char* name = "Gain";
};
}

namespace muse::audio::engine {
//! NOTE A flat, non-automated input-trim gain applied to a track's own signal
//! before its Audio FX chain (unlike ControlNode/AutomationControlNode's
//! volume+pan, which are applied after the FX chain - see TrackChain::rebuild())
class GainNode : public AudioNode<GainTag>
{
public:

    void setGain(float gainDb);
    float gain() const;

protected:

    void doSelfProcess(float* buffer, samples_t samplesPerChannel) override;

    float m_gainDb = 0.f;
    float m_linearGain = 1.f;
};

using GainNodePtr = std::shared_ptr<GainNode>;
}
