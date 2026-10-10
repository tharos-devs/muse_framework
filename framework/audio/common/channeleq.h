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

static constexpr size_t MAX_BAND_SECTIONS = 1;

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

//! NOTE Pass filters have no gain; neither they nor the first shelves have a Q
inline bool bandTypeHasGain(EqBandType type)
{
    return !isEqPassType(type);
}

inline bool bandTypeHasQ(EqBandType type)
{
    return !isEqPassType(type) && type != EqBandType::LowShelf1 && type != EqBandType::HighShelf1;
}

//! NOTE The shown Q is a musical value, each type turns it into its filter's own Q:
//! - Parametric I: a bell, Q / 2
//! - Parametric II: a bell narrow for small boosts/cuts, widening for large ones
//! - Shelf I: 6 dB/octave, the frequency is where it's 3 dB away from its flat side
//! - Shelf II, III, IV: 12 dB/octave, centred on the frequency; Q steepens its flat side (II, with a slight
//!   overshoot away from the gain there), its gain side (III, with a slight overshoot beyond the gain), or both (IV)
//! - Pass I and II: 12 dB/octave, each with its own fixed shape (rounded knee), Q unused
namespace detail {
static constexpr double HIGH_PASS1_Q = 0.62;
static constexpr double HIGH_PASS2_Q = 0.50;
static constexpr double LOW_PASS1_Q = 0.30;
static constexpr double LOW_PASS2_Q = 0.53;

static constexpr double SHELF_Q = 0.52;
static constexpr double SHELF_Q_EXPONENT = 0.19;

inline double parametric2Q(double q, double gainDb)
{
    return std::clamp(2.98 * std::pow(q, 0.84) * std::pow(10.0, -std::abs(gainDb) / 22.0), 0.05, 40.0);
}

struct Prepared {
    double cosw = 1.0;
    double alpha = 0.0;
};

inline double safeFrequency(double frequency, double sampleRate)
{
    return std::clamp(frequency, 10.0, 0.45 * sampleRate);
}

inline Prepared prepare(double frequency, double q, double sampleRate)
{
    const double w0 = 2.0 * PI * safeFrequency(frequency, sampleRate) / sampleRate;
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

//! NOTE A first order shelf (bilinear transform), its frequency being where it's 3 dB (or half its gain, when
//! smaller than 6 dB) away from its flat side
inline Biquad firstOrderShelf(bool low, double frequency, double gainDb, double sampleRate)
{
    const double g = std::pow(10.0, gainDb / 20.0);
    const double markDb = std::min(3.0, std::abs(gainDb) / 2.0);
    const double t = std::pow(10.0, std::copysign(markDb, gainDb) / 20.0);
    if (std::abs(g - t) < 1e-9 || std::abs(t - 1.0) < 1e-9) {
        return Biquad();
    }

    // the pole, relative to the frequency
    const double ratio = low ? std::sqrt((t * t - 1.0) / (g * g - t * t))
                         : std::sqrt((g * g - t * t) / (t * t - 1.0));
    const double wp = std::tan(PI * std::min(safeFrequency(frequency, sampleRate) * ratio, 0.45 * sampleRate) / sampleRate);

    if (low) {
        // H(s) = (s + g wp) / (s + wp)
        const double wz = g * wp;
        return normalized(1.0 + wz, wz - 1.0, 0.0, 1.0 + wp, wp - 1.0, 0.0);
    }

    // H(s) = (g s + wp) / (s + wp)
    return normalized(g + wp, wp - g, 0.0, 1.0 + wp, wp - 1.0, 0.0);
}

//! NOTE A second order shelf centred on the frequency, with its own Q for its zeros and its poles (bilinear
//! transform, prewarped at the frequency): for a low shelf, H(s) = (s² + s wz/qz + wz²) / (s² + s wp/qp + wp²),
//! wz/wp = sqrt(gain); a high shelf is its mirror image around the frequency
inline Biquad secondOrderShelf(bool low, double frequency, double gainDb, double qZeros, double qPoles, double sampleRate)
{
    const double g = std::pow(10.0, gainDb / 20.0);
    const double wz = std::pow(g, 0.25); // relative to the frequency
    const double wp = std::pow(g, -0.25);

    // numerator and denominator: c2 s² + c1 s + c0
    double n2 = 1.0, n1 = wz / qZeros, n0 = wz * wz;
    double d2 = 1.0, d1 = wp / qPoles, d0 = wp * wp;
    if (!low) {
        // s -> 1/s
        std::swap(n2, n0);
        std::swap(d2, d0);
    }

    const double c = 1.0 / std::tan(PI * safeFrequency(frequency, sampleRate) / sampleRate);
    const double cc = c * c;

    return normalized(n2 * cc + n1 * c + n0, 2.0 * (n0 - n2 * cc), n2 * cc - n1 * c + n0,
                      d2 * cc + d1 * c + d0, 2.0 * (d0 - d2 * cc), d2 * cc - d1 * c + d0);
}

inline double shelfQ(double q)
{
    return SHELF_Q * std::pow(std::max(q, 0.01), SHELF_Q_EXPONENT);
}

//! NOTE The flat side's singularities are the zeros when boosting, the poles when cutting
inline Biquad shelf(bool low, double frequency, double gainDb, double flatSideQ, double gainSideQ, double sampleRate)
{
    const bool boost = gainDb >= 0.0;
    return secondOrderShelf(low, frequency, gainDb, boost ? flatSideQ : gainSideQ, boost ? gainSideQ : flatSideQ, sampleRate);
}
}

inline BandFilter bandFilter(const EqBandParams& band, double sampleRate)
{
    using namespace detail;

    BandFilter filter;
    filter.sectionCount = 1;
    Biquad& section = filter.sections[0];

    const double f = band.frequency;
    const double g = band.gain;
    const double q = band.q;

    switch (band.type) {
    case EqBandType::Parametric1:
        section = peaking(f, g, q / 2.0, sampleRate);
        break;
    case EqBandType::Parametric2:
        section = peaking(f, g, parametric2Q(q, g), sampleRate);
        break;
    case EqBandType::LowShelf1:
    case EqBandType::HighShelf1:
        section = firstOrderShelf(band.type == EqBandType::LowShelf1, f, g, sampleRate);
        break;
    case EqBandType::LowShelf2:
        section = shelf(true, f, g, shelfQ(q), SHELF_Q, sampleRate);
        break;
    case EqBandType::LowShelf3:
        section = shelf(true, f, g, SHELF_Q, shelfQ(q), sampleRate);
        break;
    case EqBandType::LowShelf4:
        section = shelf(true, f, g, shelfQ(q), shelfQ(q), sampleRate);
        break;
    case EqBandType::HighShelf2:
        section = shelf(false, f, g, shelfQ(q), SHELF_Q, sampleRate);
        break;
    case EqBandType::HighShelf3:
        section = shelf(false, f, g, SHELF_Q, shelfQ(q), sampleRate);
        break;
    case EqBandType::HighShelf4:
        section = shelf(false, f, g, shelfQ(q), shelfQ(q), sampleRate);
        break;
    case EqBandType::HighPass1:
        section = highPass(f, HIGH_PASS1_Q, sampleRate);
        break;
    case EqBandType::HighPass2:
        section = highPass(f, HIGH_PASS2_Q, sampleRate);
        break;
    case EqBandType::LowPass1:
        section = lowPass(f, LOW_PASS1_Q, sampleRate);
        break;
    case EqBandType::LowPass2:
        section = lowPass(f, LOW_PASS2_Q, sampleRate);
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
