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

#include <CoreAudio/CoreAudio.h>

namespace muse::audio {
//! NOTE What an output device adds after its IO, until the sound is heard: its own latency, its safety offset
//! and its output stream's latency, as it declares them (0 if unknown)
inline double osxDeviceOutputLatencySecs(AudioObjectID deviceId)
{
    auto uint32Property = [](AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
        const AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMaster };
        UInt32 value = 0;
        UInt32 size = sizeof(value);
        if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr) {
            return UInt32(0);
        }
        return value;
    };

    UInt32 frames = uint32Property(deviceId, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)
                    + uint32Property(deviceId, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);

    const AudioObjectPropertyAddress streamsAddress = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                                        kAudioObjectPropertyElementMaster };
    AudioStreamID stream = 0;
    UInt32 streamSize = sizeof(stream);
    if (AudioObjectGetPropertyData(deviceId, &streamsAddress, 0, nullptr, &streamSize, &stream) == noErr && streamSize >= sizeof(stream)) {
        frames += uint32Property(stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    }

    const AudioObjectPropertyAddress rateAddress = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMaster };
    Float64 rate = 0.0;
    UInt32 rateSize = sizeof(rate);
    if (AudioObjectGetPropertyData(deviceId, &rateAddress, 0, nullptr, &rateSize, &rate) != noErr || rate <= 0.0) {
        return 0.0;
    }

    return frames / rate;
}
}
