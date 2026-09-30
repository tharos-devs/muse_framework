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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <thread>

#include "audio/common/audiosanitizer.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

namespace {
constexpr float INT16_TO_FLOAT = 1.f / 32768.f;
constexpr double CROSSFADE_SECS = 0.005;

//! NOTE A gap longer than this since the previous block means the rendering stopped in between
//! (pause, stop, mute, not processed while idle...): the previous position is then stale
constexpr int64_t RENDERING_STOPPED_AFTER_US = 100'000;

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

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

SoundTrackAudioNode::SoundTrackAudioNode()
    : m_loader(std::make_shared<Loader>())
{
    setName("Source[SoundTrack]");
}

std::shared_ptr<const SoundTrackAudioNode::Data> SoundTrackAudioNode::loadFile(const std::string& path)
{
    //! NOTE The path is UTF-8: going through std::filesystem::path makes it work with non-ASCII paths
    //! on Windows too, where a narrow std::string path would be read in the ANSI code page
    const std::u8string u8path(reinterpret_cast<const char8_t*>(path.data()), path.size());
    std::ifstream file(std::filesystem::path(u8path), std::ios::binary);
    if (!file) {
        LOGE() << "failed to open sound track file: " << path;
        return nullptr;
    }

    char riff[12];
    if (!file.read(riff, 12) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        LOGE() << "not a WAV file: " << path;
        return nullptr;
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
            //! NOTE Only the plain 16-bit PCM written by the app's own decoder is supported; more channels
            //! than the output has are dropped, not downmixed (the decoder always writes stereo)
            if (format != 1 || bitsPerSample != 16 || channels == 0 || sampleRate == 0) {
                LOGE() << "unsupported WAV format (16-bit PCM expected): " << path;
                return nullptr;
            }

            //! NOTE Never trust the header's size more than the file itself (truncated file)
            const std::streampos dataStart = file.tellg();
            file.seekg(0, std::ios::end);
            const std::streamoff remaining = file.tellg() - dataStart;
            file.seekg(dataStart);
            const uint64_t dataBytes = std::min<uint64_t>(chunkSize, remaining > 0 ? static_cast<uint64_t>(remaining) : 0);

            auto data = std::make_shared<Data>();
            try {
                data->samples.resize(static_cast<size_t>(dataBytes / sizeof(int16_t)));
            } catch (const std::bad_alloc&) {
                LOGE() << "not enough memory to load the sound track: " << path << " (" << chunkSize << " bytes)";
                return nullptr;
            }

            file.read(reinterpret_cast<char*>(data->samples.data()), static_cast<std::streamsize>(data->samples.size() * sizeof(int16_t)));
            data->samples.resize(static_cast<size_t>(file.gcount()) / sizeof(int16_t));

            data->channels = channels;
            data->sampleRate = sampleRate;
            data->frames = static_cast<int64_t>(data->samples.size() / channels);

            LOGI() << "sound track loaded: " << path << ", " << data->frames << " frames, "
                   << channels << " ch, " << sampleRate << " Hz";
            return data;
        }

        file.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
    }

    LOGE() << "no data chunk in WAV file: " << path;
    return nullptr;
}

void SoundTrackAudioNode::requestLoad(const std::string& path)
{
    ONLY_AUDIO_ENGINE_THREAD;

    m_requestedPath = path;

    std::shared_ptr<Loader> loader = m_loader;

    {
        std::lock_guard lock(loader->mutex);
        loader->requestedPath = path;

        if (path.empty()) {
            loader->publish(nullptr);
            loader->loading = false;
            return;
        }

        loader->loading = true;
    }

    std::thread([loader, path]() {
        std::shared_ptr<const Data> data = loadFile(path);

        std::lock_guard lock(loader->mutex);
        if (loader->requestedPath == path) { // not superseded by another request in the meantime
            loader->publish(data);
            loader->loading = false;
        }
    }).detach();
}

bool SoundTrackAudioNode::isLoading() const
{
    return m_loader->loading.load();
}

void SoundTrackAudioNode::Loader::publish(const std::shared_ptr<const Data>& data)
{
    // Called with `mutex` locked
    std::shared_ptr<const Data> previous = current;
    current = data;
    active.store(data.get()); // seq_cst, paired with the reader's increment + load (see doSelfProcess())

    //! NOTE A reader that counted itself before the store above may still use the previous data: wait for
    //! it (at most the end of one audio block). One that counts itself after it reads the new data.
    while (readers.load() != 0) {
        std::this_thread::yield();
    }

    previous.reset();
}

void SoundTrackAudioNode::setPlayheadPosition(const PlayheadPositionPtr& playheadPosition)
{
    m_playheadPosition = playheadPosition;
}

void SoundTrackAudioNode::setForceIncludeInExport(bool force)
{
    ONLY_AUDIO_ENGINE_THREAD;
    m_forceIncludeInExport = force;
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
    ONLY_AUDIO_ENGINE_THREAD;

    //! NOTE Called on pause/stop: whatever comes next doesn't continue the previous block
    m_wasRendering = false;
    m_lastStartSample = -1;
}

const AudioInputParams& SoundTrackAudioNode::inputParams() const
{
    return m_params;
}

void SoundTrackAudioNode::applyInputParams(const AudioInputParams& requiredParams)
{
    ONLY_AUDIO_ENGINE_THREAD;

    //! NOTE Compared to the requested path (not the loaded one), so a file that fails to load isn't
    //! retried on every params change
    const std::string path = configValue(requiredParams.configuration, SOUND_TRACK_FILE_PATH_KEY);
    if (path != m_requestedPath) {
        requestLoad(path);
    }

    const std::string offset = configValue(requiredParams.configuration, SOUND_TRACK_OFFSET_MS_KEY);
    m_offsetMs = offset.empty() ? 0 : std::strtoll(offset.c_str(), nullptr, 10);

    m_includeInExport = configValue(requiredParams.configuration, SOUND_TRACK_INCLUDE_IN_EXPORT_KEY) == "1";

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
    //! NOTE Never waited for: the track is just silent until its file is loaded
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
    m_wasRendering = false;
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
    const bool followsPlayhead = currentMode == ProcessMode::Playing && m_playheadPosition && outRate > 0;
    if (followsPlayhead) {
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

    const int64_t now = nowUs();
    if (m_wasRendering && now - m_lastRenderTimeUs > RENDERING_STOPPED_AFTER_US) {
        m_wasRendering = false;
    }
    m_lastRenderTimeUs = now;

    //! NOTE Counted as a reader until the end of the block, so `data` can't be released meanwhile
    struct ReaderScope {
        std::atomic<int>& readers;
        explicit ReaderScope(std::atomic<int>& r)
            : readers(r) { readers.fetch_add(1); }
        ~ReaderScope() { readers.fetch_sub(1); }
    } readerScope(m_loader->readers);

    const Data* data = m_loader->active.load();
    if (data != m_lastData) {
        m_lastData = data;
        m_wasRendering = false;
    }

    if ((currentMode == ProcessMode::PlayingOffline && !m_includeInExport && !m_forceIncludeInExport)
        || !data || data->frames <= 0 || outRate == 0 || outChannels == 0) {
        m_wasRendering = false;
        m_lastStartSample = startSample;
        return;
    }

    const samples_t rampFrames = std::min<samples_t>(samplesPerChannel, static_cast<samples_t>(outRate * CROSSFADE_SECS));

    //! NOTE The playhead can stay still while playing (e.g. at the end of the score, until the engine
    //! pauses): rendering the same block again would repeat it, so fade out once and stay silent
    if (followsPlayhead && startSample == m_lastStartSample) {
        if (m_wasRendering) {
            mixFrom(*data, buffer, rampFrames, m_expectedFilePos, 1.f, 0.f, rampFrames);
            m_wasRendering = false;
        }
        return;
    }
    m_lastStartSample = startSample;

    // File frame corresponding to the first output frame
    const double startFilePos = (static_cast<double>(startSample) / outRate + m_offsetMs / 1000.0) * data->sampleRate;

    //! NOTE Any jump of the read position (seek or loop wrap while playing, offset change, start of
    //! playback) is crossfaded over a few ms instead of cutting the waveform abruptly, which clicks.
    //! The continuation of the previous position is still in the file, so it can be faded out.
    const bool continuous = m_wasRendering && std::abs(startFilePos - m_expectedFilePos) < 0.5;

    if (continuous) {
        mixFrom(*data, buffer, samplesPerChannel, startFilePos, 1.f, 1.f, 0);
    } else {
        mixFrom(*data, buffer, samplesPerChannel, startFilePos, 0.f, 1.f, rampFrames);

        if (m_wasRendering) {
            mixFrom(*data, buffer, rampFrames, m_expectedFilePos, 1.f, 0.f, rampFrames);
        }
    }

    const double step = static_cast<double>(data->sampleRate) / outRate;
    m_expectedFilePos = startFilePos + samplesPerChannel * step;
    m_wasRendering = true;
}

void SoundTrackAudioNode::mixFrom(const Data& data, float* buffer, samples_t frames, double startFilePos, float gainFrom,
                                  float gainTo, samples_t rampFrames) const
{
    const sample_rate_t outRate = m_outputSpec.sampleRate;
    const audioch_t outChannels = m_outputSpec.audioChannelCount;

    // File frames per output frame
    const double step = static_cast<double>(data.sampleRate) / outRate;

    const int16_t* samples = data.samples.data();
    const audioch_t fileChannels = data.channels;
    const bool sameRate = data.sampleRate == outRate;
    const int64_t startFrameInt = static_cast<int64_t>(std::llround(startFilePos));

    for (samples_t i = 0; i < frames; ++i) {
        float* out = buffer + static_cast<size_t>(i) * outChannels;

        const float gain = i < rampFrames
                           ? gainFrom + (gainTo - gainFrom) * (static_cast<float>(i) + 0.5f) / static_cast<float>(rampFrames)
                           : gainTo;
        const float scale = gain * INT16_TO_FLOAT;

        if (sameRate) {
            const int64_t frame = startFrameInt + i;
            if (frame < 0 || frame >= data.frames) {
                continue;
            }

            const int16_t* in = samples + frame * fileChannels;
            for (audioch_t ch = 0; ch < outChannels; ++ch) {
                out[ch] += in[std::min<audioch_t>(ch, fileChannels - 1)] * scale;
            }
            continue;
        }

        //! NOTE Linear interpolation: only used when the file's rate differs from the output's (the app
        //! decodes at the engine's rate, so in practice only for an export at another sample rate)
        const double pos = startFilePos + i * step;
        const double floorPos = std::floor(pos);
        const int64_t frame = static_cast<int64_t>(floorPos);
        if (frame < 0 || frame + 1 >= data.frames) {
            continue;
        }

        const float frac = static_cast<float>(pos - floorPos);
        const int16_t* a = samples + frame * fileChannels;
        const int16_t* b = a + fileChannels;
        for (audioch_t ch = 0; ch < outChannels; ++ch) {
            const audioch_t fileCh = std::min<audioch_t>(ch, fileChannels - 1);
            out[ch] += (a[fileCh] + (b[fileCh] - a[fileCh]) * frac) * scale;
        }
    }
}
