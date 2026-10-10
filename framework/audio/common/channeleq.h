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

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

#include "audiotypes.h"

//! NOTE The channel EQ's filters, used both by the audio engine (EqNode) and by the displayed curves,
//! so that what is shown is what is heard. Second order sections from the Audio EQ Cookbook (R. Bristow-Johnson)
namespace muse::audio::eq {
// M_PI needs _USE_MATH_DEFINES with MSVC
static constexpr double PI = 3.14159265358979323846;

struct Biquad {
    // normalized: a0 = 1
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

static constexpr size_t MAX_BAND_SECTIONS = 3;

struct BandFilter {
    std::array<Biquad, MAX_BAND_SECTIONS> sections;
    size_t sectionCount = 0;
};

//! NOTE The types a band can take, in the order of its type menu
inline std::vector<EqBandType> availableBandTypes(size_t bandIndex)
{
    if (bandIndex == 0) {
        return { EqBandType::Parametric1, EqBandType::LowShelf1, EqBandType::HighPass1, EqBandType::HighPass2,
                 EqBandType::Parametric2, EqBandType::LowShelf2, EqBandType::LowShelf3, EqBandType::LowShelf4 };
    }

    if (bandIndex == EQ_BAND_COUNT - 1) {
        return { EqBandType::Parametric1, EqBandType::HighShelf1, EqBandType::LowPass1, EqBandType::LowPass2,
                 EqBandType::Parametric2, EqBandType::HighShelf2, EqBandType::HighShelf3, EqBandType::HighShelf4 };
    }

    return { EqBandType::Parametric1, EqBandType::Parametric2 };
}

//! NOTE Pass filters have no gain; plain shelves and steep pass filters have no Q
inline bool bandTypeHasGain(EqBandType type)
{
    return !isEqPassType(type);
}

inline bool bandTypeHasQ(EqBandType type)
{
    return type != EqBandType::LowShelf1 && type != EqBandType::HighShelf1
           && type != EqBandType::HighPass2 && type != EqBandType::LowPass2;
}

namespace detail {
static constexpr double BUTTERWORTH_Q = 0.7071067811865476;
// the two sections of a 4th order Butterworth filter
static constexpr double BUTTERWORTH4_Q1 = 0.5411961001461970;
static constexpr double BUTTERWORTH4_Q2 = 1.3065629648763766;

// share of the shelf's gain given to its dip/bump, and its distance to the corner frequency
static constexpr double SHELF_ACCENT_AMOUNT = 0.25;
static constexpr double SHELF_ACCENT_OCTAVES = 1.0;
static constexpr double SHELF_ACCENT_Q = 1.2;

struct Prepared {
    double cosw = 1.0;
    double alpha = 0.0;
};

inline Prepared prepare(double frequency, double q, double sampleRate)
{
    const double nyquistSafe = 0.45 * sampleRate;
    const double f = std::clamp(frequency, 10.0, nyquistSafe);
    const double w0 = 2.0 * PI * f / sampleRate;
    return { std::cos(w0), std::sin(w0) / (2.0 * std::max(q, 0.01)) };
}

inline Biquad normalized(double b0, double b1, double b2, double a0, double a1, double a2)
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

inline Biquad peaking(double frequency, double gainDb, double q, double sampleRate)
{
    const Prepared p = prepare(frequency, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    return normalized(1.0 + p.alpha * a, -2.0 * p.cosw, 1.0 - p.alpha * a,
                      1.0 + p.alpha / a, -2.0 * p.cosw, 1.0 - p.alpha / a);
}

inline Biquad lowShelf(double frequency, double gainDb, double q, double sampleRate)
{
    const Prepared p = prepare(frequency, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double k = 2.0 * std::sqrt(a) * p.alpha;
    return normalized(a * ((a + 1.0) - (a - 1.0) * p.cosw + k),
                      2.0 * a * ((a - 1.0) - (a + 1.0) * p.cosw),
                      a * ((a + 1.0) - (a - 1.0) * p.cosw - k),
                      (a + 1.0) + (a - 1.0) * p.cosw + k,
                      -2.0 * ((a - 1.0) + (a + 1.0) * p.cosw),
                      (a + 1.0) + (a - 1.0) * p.cosw - k);
}

inline Biquad highShelf(double frequency, double gainDb, double q, double sampleRate)
{
    const Prepared p = prepare(frequency, q, sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double k = 2.0 * std::sqrt(a) * p.alpha;
    return normalized(a * ((a + 1.0) + (a - 1.0) * p.cosw + k),
                      -2.0 * a * ((a - 1.0) + (a + 1.0) * p.cosw),
                      a * ((a + 1.0) + (a - 1.0) * p.cosw - k),
                      (a + 1.0) - (a - 1.0) * p.cosw + k,
                      2.0 * ((a - 1.0) - (a + 1.0) * p.cosw),
                      (a + 1.0) - (a - 1.0) * p.cosw - k);
}

inline Biquad highPass(double frequency, double q, double sampleRate)
{
    const Prepared p = prepare(frequency, q, sampleRate);
    return normalized((1.0 + p.cosw) / 2.0, -(1.0 + p.cosw), (1.0 + p.cosw) / 2.0,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

inline Biquad lowPass(double frequency, double q, double sampleRate)
{
    const Prepared p = prepare(frequency, q, sampleRate);
    return normalized((1.0 - p.cosw) / 2.0, 1.0 - p.cosw, (1.0 - p.cosw) / 2.0,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

//! NOTE Shelf II adds a dip opposite to the gain past the corner, Shelf III a bump in the gain's direction before it,
//! Shelf IV both; Q scales how pronounced they are
inline void addShelf(BandFilter& filter, bool low, int variant, const EqBandParams& band, double sampleRate)
{
    const double f = band.frequency;
    const double g = band.gain;

    filter.sections[filter.sectionCount++] = low ? lowShelf(f, g, BUTTERWORTH_Q, sampleRate)
                                             : highShelf(f, g, BUTTERWORTH_Q, sampleRate);

    if (variant == 1) {
        return;
    }

    const double amount = SHELF_ACCENT_AMOUNT * std::clamp(static_cast<double>(band.q), 0.0, 2.4);
    const double outside = std::pow(2.0, low ? SHELF_ACCENT_OCTAVES : -SHELF_ACCENT_OCTAVES);

    const bool dip = variant == 2 || variant == 4;
    const bool bump = variant == 3 || variant == 4;

    if (dip) {
        filter.sections[filter.sectionCount++] = peaking(f * outside, -g * amount, SHELF_ACCENT_Q, sampleRate);
    }
    if (bump) {
        filter.sections[filter.sectionCount++] = peaking(f / outside, g * amount, SHELF_ACCENT_Q, sampleRate);
    }
}
}

inline BandFilter bandFilter(const EqBandParams& band, double sampleRate)
{
    using namespace detail;

    BandFilter filter;
    const double f = band.frequency;
    const double g = band.gain;
    const double q = band.q;

    switch (band.type) {
    case EqBandType::Parametric1:
        filter.sections[filter.sectionCount++] = peaking(f, g, q, sampleRate);
        break;
    case EqBandType::Parametric2:
        // narrows as the gain grows
        filter.sections[filter.sectionCount++] = peaking(f, g, q * std::pow(2.0, std::abs(g) / 12.0), sampleRate);
        break;
    case EqBandType::LowShelf1: addShelf(filter, true, 1, band, sampleRate);
        break;
    case EqBandType::LowShelf2: addShelf(filter, true, 2, band, sampleRate);
        break;
    case EqBandType::LowShelf3: addShelf(filter, true, 3, band, sampleRate);
        break;
    case EqBandType::LowShelf4: addShelf(filter, true, 4, band, sampleRate);
        break;
    case EqBandType::HighShelf1: addShelf(filter, false, 1, band, sampleRate);
        break;
    case EqBandType::HighShelf2: addShelf(filter, false, 2, band, sampleRate);
        break;
    case EqBandType::HighShelf3: addShelf(filter, false, 3, band, sampleRate);
        break;
    case EqBandType::HighShelf4: addShelf(filter, false, 4, band, sampleRate);
        break;
    case EqBandType::HighPass1:
        filter.sections[filter.sectionCount++] = highPass(f, q, sampleRate);
        break;
    case EqBandType::HighPass2:
        filter.sections[filter.sectionCount++] = highPass(f, BUTTERWORTH4_Q1, sampleRate);
        filter.sections[filter.sectionCount++] = highPass(f, BUTTERWORTH4_Q2, sampleRate);
        break;
    case EqBandType::LowPass1:
        filter.sections[filter.sectionCount++] = lowPass(f, q, sampleRate);
        break;
    case EqBandType::LowPass2:
        filter.sections[filter.sectionCount++] = lowPass(f, BUTTERWORTH4_Q1, sampleRate);
        filter.sections[filter.sectionCount++] = lowPass(f, BUTTERWORTH4_Q2, sampleRate);
        break;
    }

    return filter;
}

//! NOTE Whether the band leaves the signal unchanged
inline bool isBandNeutral(const EqBandParams& band)
{
    return !band.enabled || (!isEqPassType(band.type) && std::abs(band.gain) < 0.001f);
}

inline double magnitudeDb(const BandFilter& filter, double frequency, double sampleRate)
{
    const double w = 2.0 * PI * frequency / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;

    double db = 0.0;
    for (size_t i = 0; i < filter.sectionCount; ++i) {
        const Biquad& s = filter.sections[i];
        const std::complex<double> h = (s.b0 + s.b1 * z1 + s.b2 * z2) / (1.0 + s.a1 * z1 + s.a2 * z2);
        db += 20.0 * std::log10(std::max(std::abs(h), 1e-9));
    }

    return db;
}

//! NOTE The whole EQ's response at a frequency, in dB
inline double responseDb(const EqParams& params, double frequency, double sampleRate)
{
    if (!params.enabled) {
        return 0.0;
    }

    double db = 0.0;
    for (const EqBandParams& band : params.bands) {
        if (!isBandNeutral(band)) {
            db += magnitudeDb(bandFilter(band, sampleRate), frequency, sampleRate);
        }
    }

    return db;
}
}
