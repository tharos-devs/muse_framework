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

#include "gainnode.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

void GainNode::setGain(float gainDb)
{
    m_gainDb = gainDb;
    m_linearGain = muse::db_to_linear(gainDb);
}

float GainNode::gain() const
{
    return m_gainDb;
}

void GainNode::doSelfProcess(float* buffer, samples_t samplesPerChannel)
{
    const audioch_t channelsCount = m_outputSpec.audioChannelCount;
    const size_t totalSamples = static_cast<size_t>(samplesPerChannel) * channelsCount;

    for (size_t i = 0; i < totalSamples; ++i) {
        buffer[i] *= m_linearGain;
    }
}
