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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audiosourcenode.h"

#include "audio/common/audiotypes.h"
#include "../../iplayhead.h"

namespace muse::audio::engine {
//! NOTE A source playing a pre-decoded 16-bit PCM WAV file (e.g. the audio track of an
//! attached video) on the engine's own timeline, so it is mixed and positioned exactly like
//! instrument tracks. The file path, offset and export flag come from the source params'
//! configuration (see SOUND_TRACK_*_KEY in audiotypes.h).
//! The whole file is loaded in memory by a background thread when its path changes, then handed
//! over atomically: neither the audio thread nor the engine thread ever waits for the disk (the
//! track is silent until it's loaded).
class SoundTrackAudioNode : public AudioSourceNode
{
public:
    SoundTrackAudioNode();

    void seek(const TimePosition& position, const bool flushSound = true) override;
    void flush() override;

    const AudioInputParams& inputParams() const override;
    void applyInputParams(const AudioInputParams& requiredParams) override;
    async::Channel<AudioInputParams> inputParamsChanged() const override;

    void prepareToPlay() override;
    bool readyToPlay() const override;
    async::Notification readyToPlayChanged() const override;

    bool hasPendingChunks() const override;
    void processInput() override;
    InputProcessingProgress inputProcessingProgress() const override;

    void clearCache() override;

    //! NOTE While playing, the read position is taken from the playhead itself on every block (not from
    //! a cursor of our own), so this track can't drift from the engine clock, whatever happened before
    //! (loops, seeks, count-in, previous playbacks...)
    void setPlayheadPosition(const PlayheadPositionPtr& playheadPosition);

    //! NOTE Export: play even if the track's own export flag is off
    void setForceIncludeInExport(bool force);

    //! NOTE Whether the requested file is still being loaded (an export must wait for it)
    bool isLoading() const;

private:
    struct Data {
        std::vector<int16_t> samples; // interleaved
        audioch_t channels = 0;
        sample_rate_t sampleRate = 0;
        int64_t frames = 0;
    };

    //! NOTE Shared with the loading thread, which may outlive the node
    struct Loader {
        std::mutex mutex;
        std::string requestedPath;
        std::shared_ptr<const Data> current;

        //! NOTE Read by the audio thread without any lock: it counts itself in `readers` for the whole
        //! block, and a replaced Data is only released once no reader may still use it (see publish())
        std::atomic<const Data*> active { nullptr };
        std::atomic<int> readers { 0 };
        std::atomic<bool> loading { false };

        void publish(const std::shared_ptr<const Data>& data);
    };

    static std::shared_ptr<const Data> loadFile(const std::string& path);
    void requestLoad(const std::string& path);

    void onOutputSpecChanged(const OutputSpec& spec) override;
    void doSelfProcess(float* buffer, samples_t samplesPerChannel) override;

    //! NOTE Adds `frames` frames read from startFilePos (in file frames), with a linear gain ramp from
    //! gainFrom to gainTo over the first rampFrames frames (gainTo afterwards)
    void mixFrom(const Data& data, float* buffer, samples_t frames, double startFilePos, float gainFrom, float gainTo,
                 samples_t rampFrames) const;

    AudioInputParams m_params;
    async::Channel<AudioInputParams> m_paramsChanges;
    async::Notification m_readyToPlayChanged;

    std::shared_ptr<Loader> m_loader;
    std::string m_requestedPath;

    int64_t m_offsetMs = 0;
    bool m_includeInExport = false;
    bool m_forceIncludeInExport = false;

    PlayheadPositionPtr m_playheadPosition;

    //! NOTE The engine timeline position of the next frame to render, in output samples.
    //! Only used when rendering offline (export), where the playhead doesn't move
    int64_t m_timelineSamples = 0;
    sample_rate_t m_lastSampleRate = 0;

    //! NOTE Where the previous block ended in the file, to detect jumps and crossfade them. Only valid if
    //! that block was the one just before: m_wasRendering is cleared whenever the rendering stops
    //! (pause, stop, mute, idle...), which is detected from the time elapsed since the last block
    bool m_wasRendering = false;
    double m_expectedFilePos = 0.0;
    int64_t m_lastStartSample = -1;
    int64_t m_lastRenderTimeUs = 0;
    const Data* m_lastData = nullptr;
};

using SoundTrackAudioNodePtr = std::shared_ptr<SoundTrackAudioNode>;
}
