/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
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

#include <vector>

#include "audionode.h"
#include "audio/common/channeleq.h"

namespace muse::audio {
struct EqTag
{
    static constexpr const char* name = "Eq";
};
}

namespace muse::audio::engine {
//! NOTE The channel EQ, applied to a track's signal right after its gain, before its Audio FX chain
//! (see TrackChain::rebuild()). Frequency, gain and Q glide to their new values, so that moving a band
//! while playing doesn't click
class EqNode : public AudioNode<EqTag>
{
public:

    void setParams(const EqParams& unsafeParams);
    const EqParams& params() const;

protected:

    void onOutputSpecChanged(const OutputSpec& spec) override;
    void doSelfProcess(float* buffer, samples_t samplesPerChannel) override;

private:
    struct BandState {
        EqBandParams current;
        eq::BandFilter filter;
        bool neutral = true;
    };

    void resetBand(size_t bandIndex);
    bool glide(size_t bandIndex);
    void resetStates(size_t bandIndex);

    EqParams m_target;
    std::array<BandState, EQ_BAND_COUNT> m_bands;
    bool m_gliding = false;

    // [band][section][channel] -> z1, z2
    std::vector<double> m_states;
};

using EqNodePtr = std::shared_ptr<EqNode>;
}
