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
#include "eqnode.h"

#include <algorithm>
#include <cmath>

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

// the filters follow the gliding values every this many samples
static constexpr samples_t GLIDE_BLOCK_SAMPLES = 32;
// time constant of the glide
static constexpr double GLIDE_SECONDS = 0.02;

void EqNode::setParams(const EqParams& params)
{
    if (m_target == params) {
        return;
    }

    for (size_t i = 0; i < EQ_BAND_COUNT; ++i) {
        const EqBandParams& oldBand = m_target.bands[i];
        const EqBandParams& newBand = params.bands[i];

        // nothing to glide between two filter types or from off: the band starts over with the new values
        if (newBand.type != oldBand.type || newBand.enabled != oldBand.enabled) {
            m_target.bands[i] = newBand;
            resetBand(i);
        }
    }

    const bool reenabled = params.enabled && !m_target.enabled;
    m_target = params;
    m_gliding = true;

    // left untouched while off: starts over from the current values
    if (reenabled) {
        for (size_t i = 0; i < EQ_BAND_COUNT; ++i) {
            resetBand(i);
        }
    }
}

const EqParams& EqNode::params() const
{
    return m_target;
}

void EqNode::onOutputSpecChanged(const OutputSpec& spec)
{
    m_states.assign(EQ_BAND_COUNT * eq::MAX_BAND_SECTIONS * spec.audioChannelCount * 2, 0.0);

    for (size_t i = 0; i < EQ_BAND_COUNT; ++i) {
        resetBand(i);
    }
}

void EqNode::resetBand(size_t bandIndex)
{
    BandState& band = m_bands[bandIndex];
    band.current = m_target.bands[bandIndex];
    band.neutral = eq::isBandNeutral(band.current);

    if (m_outputSpec.sampleRate > 0) {
        band.filter = eq::bandFilter(band.current, m_outputSpec.sampleRate);
    }

    resetStates(bandIndex);
}

void EqNode::resetStates(size_t bandIndex)
{
    const size_t bandSize = eq::MAX_BAND_SECTIONS * m_outputSpec.audioChannelCount * 2;
    const size_t begin = bandIndex * bandSize;
    if (begin + bandSize <= m_states.size()) {
        std::fill(m_states.begin() + begin, m_states.begin() + begin + bandSize, 0.0);
    }
}

//! NOTE Moves the band's values one step towards their targets; returns whether they're still moving
bool EqNode::glide(size_t bandIndex)
{
    BandState& band = m_bands[bandIndex];
    const EqBandParams& target = m_target.bands[bandIndex];

    if (band.current == target) {
        return false;
    }

    const double step = 1.0 - std::exp(-static_cast<double>(GLIDE_BLOCK_SAMPLES) / (GLIDE_SECONDS * m_outputSpec.sampleRate));

    // frequency glides on a log scale, like it's heard
    const double logFrequency = std::log(band.current.frequency);
    const double newLogFrequency = logFrequency + (std::log(target.frequency) - logFrequency) * step;
    band.current.frequency = static_cast<float>(std::exp(newLogFrequency));
    band.current.gain += static_cast<float>((target.gain - band.current.gain) * step);
    band.current.q += static_cast<float>((target.q - band.current.q) * step);

    const bool reached = std::abs(target.frequency - band.current.frequency) < 0.01f * target.frequency
                         && std::abs(target.gain - band.current.gain) < 0.01f
                         && std::abs(target.q - band.current.q) < 0.001f;
    if (reached) {
        band.current = target;
    }

    const bool wasNeutral = band.neutral;
    band.neutral = eq::isBandNeutral(band.current);
    if (wasNeutral && !band.neutral) {
        resetStates(bandIndex);
    }

    band.filter = eq::bandFilter(band.current, m_outputSpec.sampleRate);

    return !reached;
}

void EqNode::doSelfProcess(float* buffer, samples_t samplesPerChannel)
{
    const audioch_t channels = m_outputSpec.audioChannelCount;
    if (!m_target.enabled || channels == 0 || m_outputSpec.sampleRate == 0 || m_states.empty()) {
        return;
    }

    if (!m_gliding && m_target.isFlat()) {
        return;
    }

    const size_t bandSize = eq::MAX_BAND_SECTIONS * channels * 2;

    for (samples_t start = 0; start < samplesPerChannel; start += GLIDE_BLOCK_SAMPLES) {
        const samples_t count = std::min<samples_t>(GLIDE_BLOCK_SAMPLES, samplesPerChannel - start);

        if (m_gliding) {
            bool stillGliding = false;
            for (size_t i = 0; i < EQ_BAND_COUNT; ++i) {
                stillGliding |= glide(i);
            }
            m_gliding = stillGliding;
        }

        for (size_t bandIndex = 0; bandIndex < EQ_BAND_COUNT; ++bandIndex) {
            const BandState& band = m_bands[bandIndex];
            if (band.neutral) {
                continue;
            }

            for (size_t sectionIndex = 0; sectionIndex < band.filter.sectionCount; ++sectionIndex) {
                const eq::Biquad& s = band.filter.sections[sectionIndex];
                double* state = m_states.data() + bandIndex * bandSize + sectionIndex * channels * 2;

                for (audioch_t ch = 0; ch < channels; ++ch) {
                    double z1 = state[ch * 2];
                    double z2 = state[ch * 2 + 1];
                    float* sample = buffer + static_cast<size_t>(start) * channels + ch;

                    // transposed direct form II
                    for (samples_t i = 0; i < count; ++i) {
                        const double x = *sample;
                        const double y = s.b0 * x + z1;
                        z1 = s.b1 * x - s.a1 * y + z2;
                        z2 = s.b2 * x - s.a2 * y;
                        *sample = static_cast<float>(y);
                        sample += channels;
                    }

                    // a decaying tail would end in denormals, very slow on some CPUs
                    state[ch * 2] = std::abs(z1) < 1e-20 ? 0.0 : z1;
                    state[ch * 2 + 1] = std::abs(z2) < 1e-20 ? 0.0 : z2;
                }
            }
        }
    }
}
