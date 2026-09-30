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

#include "soundtrackaudionode.h"

#include <cmath>
#include <cstring>
#include <fstream>

#include "audio/common/audiosanitizer.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

namespace {
constexpr float INT16_TO_FLOAT = 1.f / 32768.f;
constexpr double CROSSFADE_SECS = 0.005;

std::string configValue(const AudioUnitConfig& config, const char* key)
{
    auto it = config.find(key);
    return it != config.cend() ? it->second : std::string();
}

uint32_t readLE32(const char* p)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(p[0]))
           | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8)
           | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16)
           | (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

uint16_t readLE16(const char* p)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8));
}
}

bool SoundTrackAudioNode::loadFile(const std::string& path)
{
    ONLY_AUDIO_ENGINE_THREAD;

    m_samples.clear();
    m_samples.shrink_to_fit();
    m_fileChannels = 0;
    m_fileSampleRate = 0;
    m_fileFrames = 0;
    m_loadedPath.clear();

    if (path.empty()) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LOGE() << "failed to open sound track file: " << path;
        return false;
    }

    char riff[12];
    if (!file.read(riff, 12) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        LOGE() << "not a WAV file: " << path;
        return false;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;

    char chunkHeader[8];
    while (file.read(chunkHeader, 8)) {
        const uint32_t chunkSize = readLE32(chunkHeader + 4);

        if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
            char fmt[16];
            if (chunkSize < 16 || !file.read(fmt, 16)) {
                break;
            }
            format = readLE16(fmt);
            channels = readLE16(fmt + 2);
            sampleRate = readLE32(fmt + 4);
            bitsPerSample = readLE16(fmt + 14);
            file.seekg(chunkSize - 16 + (chunkSize & 1), std::ios::cur);
            continue;
        }

        if (std::memcmp(chunkHeader, "data", 4) == 0) {
            if (format != 1 || bitsPerSample != 16 || channels == 0 || sampleRate == 0) {
                LOGE() << "unsupported WAV format (16-bit PCM expected): " << path;
                return false;
            }

            m_samples.resize(chunkSize / sizeof(int16_t));
            file.read(reinterpret_cast<char*>(m_samples.data()), static_cast<std::streamsize>(m_samples.size() * sizeof(int16_t)));
            m_samples.resize(static_cast<size_t>(file.gcount()) / sizeof(int16_t));

            m_fileChannels = channels;
            m_fileSampleRate = sampleRate;
            m_fileFrames = static_cast<int64_t>(m_samples.size() / channels);
            m_loadedPath = path;

            LOGI() << "sound track loaded: " << path << ", " << m_fileFrames << " frames, "
                   << channels << " ch, " << sampleRate << " Hz";
            return true;
        }

        file.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
    }

    LOGE() << "no data chunk in WAV file: " << path;
    return false;
}

bool SoundTrackAudioNode::isLoaded() const
{
    return m_fileFrames > 0;
}

void SoundTrackAudioNode::setPlayheadPosition(const PlayheadPositionPtr& playheadPosition)
{
    m_playheadPosition = playheadPosition;
}

void SoundTrackAudioNode::seek(const TimePosition& position, const bool)
{
    ONLY_AUDIO_ENGINE_THREAD;

    if (!position.isValid()) {
        return;
    }

    const sample_rate_t outRate = m_outputSpec.sampleRate > 0 ? m_outputSpec.sampleRate : position.sampleRate();
    m_timelineSamples = static_cast<int64_t>(std::llround(position.time().raw() * outRate));
}

void SoundTrackAudioNode::flush()
{
}

const AudioInputParams& SoundTrackAudioNode::inputParams() const
{
    return m_params;
}

void SoundTrackAudioNode::applyInputParams(const AudioInputParams& requiredParams)
{
    ONLY_AUDIO_ENGINE_THREAD;

    const std::string path = configValue(requiredParams.configuration, SOUND_TRACK_FILE_PATH_KEY);
    if (path != m_loadedPath) {
        loadFile(path);
        m_readyToPlayChanged.notify();
    }

    const std::string offset = configValue(requiredParams.configuration, SOUND_TRACK_OFFSET_SECS_KEY);
    m_offsetSecs = offset.empty() ? 0.0 : std::strtod(offset.c_str(), nullptr);

    m_includeInExport = configValue(requiredParams.configuration, SOUND_TRACK_INCLUDE_IN_EXPORT_KEY) == "1";

    setName(std::string("Source[SoundTrack]"));

    if (m_params != requiredParams) {
        m_params = requiredParams;
        m_paramsChanges.send(m_params);
    }
}

async::Channel<AudioInputParams> SoundTrackAudioNode::inputParamsChanged() const
{
    return m_paramsChanges;
}

void SoundTrackAudioNode::prepareToPlay()
{
}

bool SoundTrackAudioNode::readyToPlay() const
{
    //! NOTE The file is fully loaded in applyInputParams(), nothing to wait for
    return true;
}

async::Notification SoundTrackAudioNode::readyToPlayChanged() const
{
    return m_readyToPlayChanged;
}

bool SoundTrackAudioNode::hasPendingChunks() const
{
    return false;
}

void SoundTrackAudioNode::processInput()
{
}

InputProcessingProgress SoundTrackAudioNode::inputProcessingProgress() const
{
    return {};
}

void SoundTrackAudioNode::clearCache()
{
}

void SoundTrackAudioNode::onOutputSpecChanged(const OutputSpec& spec)
{
    ONLY_AUDIO_ENGINE_THREAD;

    //! NOTE Keep the same timeline position at the new rate (e.g. export at another sample rate)
    if (m_lastSampleRate > 0 && spec.sampleRate > 0 && spec.sampleRate != m_lastSampleRate) {
        m_timelineSamples = static_cast<int64_t>(std::llround(static_cast<double>(m_timelineSamples)
                                                              * spec.sampleRate / m_lastSampleRate));
    }

    m_lastSampleRate = spec.sampleRate;
}

void SoundTrackAudioNode::doSelfProcess(float* buffer, samples_t samplesPerChannel)
{
    ONLY_AUDIO_PROC_THREAD;

    const ProcessMode currentMode = mode();
    if (currentMode != ProcessMode::Playing && currentMode != ProcessMode::PlayingOffline) {
        m_wasRendering = false;
        return;
    }

    const sample_rate_t outRate = m_outputSpec.sampleRate;
    const audioch_t outChannels = m_outputSpec.audioChannelCount;

    int64_t startSample = m_timelineSamples;
    if (currentMode == ProcessMode::Playing && m_playheadPosition && outRate > 0) {
        //! NOTE The playhead is forwarded after the whole chain is processed (see PlayheadNode),
        //! so this is the engine position of this block's first frame
        const TimePosition& position = m_playheadPosition->currentPosition();
        if (position.isValid()) {
            startSample = position.sampleRate() == outRate
                          ? position.samples()
                          : static_cast<int64_t>(std::llround(position.time().raw() * outRate));
        }
    }

    m_timelineSamples = startSample + samplesPerChannel;

    if ((currentMode == ProcessMode::PlayingOffline && !m_includeInExport)
        || !isLoaded() || outRate == 0 || outChannels == 0) {
        m_wasRendering = false;
        return;
    }

    // File frame corresponding to the first output frame
    const double startFilePos = (static_cast<double>(startSample) / outRate + m_offsetSecs) * m_fileSampleRate;

    //! NOTE Any jump of the read position (seek or loop wrap while playing, offset change, start of
    //! playback) is crossfaded over a few ms instead of cutting the waveform abruptly, which clicks.
    //! The continuation of the previous position is still in the file, so it can be faded out.
    const samples_t rampFrames = std::min<samples_t>(samplesPerChannel, static_cast<samples_t>(outRate * CROSSFADE_SECS));
    const bool continuous = m_wasRendering && std::abs(startFilePos - m_expectedFilePos) < 0.5;

    if (continuous) {
        mixFrom(buffer, samplesPerChannel, startFilePos, 1.f, 1.f, 0);
    } else {
        mixFrom(buffer, samplesPerChannel, startFilePos, 0.f, 1.f, rampFrames);

        if (m_wasRendering) {
            mixFrom(buffer, rampFrames, m_expectedFilePos, 1.f, 0.f, rampFrames);
        }
    }

    const double step = static_cast<double>(m_fileSampleRate) / outRate;
    m_expectedFilePos = startFilePos + samplesPerChannel * step;
    m_wasRendering = true;
}

void SoundTrackAudioNode::mixFrom(float* buffer, samples_t frames, double startFilePos, float gainFrom, float gainTo,
                                  samples_t rampFrames) const
{
    const sample_rate_t outRate = m_outputSpec.sampleRate;
    const audioch_t outChannels = m_outputSpec.audioChannelCount;

    // File frames per output frame
    const double step = static_cast<double>(m_fileSampleRate) / outRate;

    const int16_t* data = m_samples.data();
    const audioch_t fileChannels = m_fileChannels;
    const bool sameRate = m_fileSampleRate == outRate;
    const int64_t startFrameInt = static_cast<int64_t>(std::llround(startFilePos));

    for (samples_t i = 0; i < frames; ++i) {
        float* out = buffer + static_cast<size_t>(i) * outChannels;

        const float gain = i < rampFrames
                           ? gainFrom + (gainTo - gainFrom) * (static_cast<float>(i) + 0.5f) / static_cast<float>(rampFrames)
                           : gainTo;
        const float scale = gain * INT16_TO_FLOAT;

        if (sameRate) {
            const int64_t frame = startFrameInt + i;
            if (frame < 0 || frame >= m_fileFrames) {
                continue;
            }

            const int16_t* in = data + frame * fileChannels;
            for (audioch_t ch = 0; ch < outChannels; ++ch) {
                out[ch] += in[std::min<audioch_t>(ch, fileChannels - 1)] * scale;
            }
            continue;
        }

        const double pos = startFilePos + i * step;
        const double floorPos = std::floor(pos);
        const int64_t frame = static_cast<int64_t>(floorPos);
        if (frame < 0 || frame + 1 >= m_fileFrames) {
            continue;
        }

        const float frac = static_cast<float>(pos - floorPos);
        const int16_t* a = data + frame * fileChannels;
        const int16_t* b = a + fileChannels;
        for (audioch_t ch = 0; ch < outChannels; ++ch) {
            const audioch_t fileCh = std::min<audioch_t>(ch, fileChannels - 1);
            out[ch] += (a[fileCh] + (b[fileCh] - a[fileCh]) * frac) * scale;
        }
    }
}
