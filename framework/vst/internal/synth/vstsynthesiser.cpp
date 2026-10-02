/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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
#include "vstsynthesiser.h"

#include <algorithm>

#include "log.h"

using namespace muse;
using namespace muse::vst;
using namespace muse::audio::synth;
using namespace muse::audio;
using namespace muse::audioplugins;

//! NOTE: every MIDI CC is mapped, not just the ones MuseScore drives itself,
//! so that raw MIDI messages (e.g. articulation changes) can reach any controller
static std::set<Steinberg::Vst::CtrlNumber> supportedControllers()
{
    std::set<Steinberg::Vst::CtrlNumber> result;
    for (Steinberg::Vst::CtrlNumber ctrl = 0; ctrl < Steinberg::Vst::kCountCtrlNumber; ++ctrl) {
        result.insert(ctrl);
    }
    result.insert(Steinberg::Vst::kCtrlProgramChange);

    return result;
}

static const std::set<Steinberg::Vst::CtrlNumber> SUPPORTED_CONTROLLERS = supportedControllers();

VstSynthesiser::VstSynthesiser(const TrackId trackId, const muse::audio::AudioInputParams& params)
    : AbstractSynthesizer(params),
    m_vstAudioClient(std::make_unique<VstAudioClient>()),
    m_trackId(trackId)
{
}

VstSynthesiser::~VstSynthesiser()
{
    instancesRegister()->unregisterInstrPlugin(m_params.resourceMeta.id, m_trackId);
}

void VstSynthesiser::init(const OutputSpec& spec)
{
    IF_ASSERT_FAILED(spec.isValid()) {
        return;
    }

    m_outputSpec = spec;

    m_pluginPtr = instancesRegister()->makeAndRegisterInstrPlugin(m_params.resourceMeta.id, m_trackId);

    m_vstAudioClient->init(PluginType::Instrument, m_pluginPtr);

    auto onPluginLoaded = [this]() {
        m_pluginPtr->updatePluginConfig(m_params.configuration);
        m_params.midiPortNames = m_vstAudioClient->eventInputBusNames();
        m_params.midiPort = std::clamp(m_params.midiPort, 0, std::max(static_cast<int>(m_params.midiPortNames.size()) - 1, 0));
        m_params.midiChannel = std::clamp(m_params.midiChannel, 0, 15);
        // Bus activation must happen while the plugin is still inactive, i.e. before setOutputSpec()
        m_vstAudioClient->setEventInputBus(m_params.midiPort);
        m_vstAudioClient->setOutputSpec(m_outputSpec);
        m_vstAudioClient->loadSupportedParams();
        initSequencer();
        m_inited = true;
        m_paramsChanges.send(m_params);
    };

    if (m_pluginPtr->isLoaded()) {
        onPluginLoaded();
    } else {
        m_pluginPtr->loadingCompleted().onNotify(this, onPluginLoaded);
    }

    m_pluginPtr->pluginSettingsChanged().onReceive(this, [this](const muse::audio::AudioUnitConfig& newConfig) {
        if (m_params.configuration == newConfig) {
            return;
        }

        m_params.configuration = newConfig;
        m_paramsChanges.send(m_params);
    });

    m_sequencer.setOnOffStreamFlushed([this]() {
        m_vstAudioClient->flushSound();
    });
}

void VstSynthesiser::initSequencer()
{
    //! NOTE: every channel, since the articulations of a map may each play on their own channel
    static constexpr int MIDI_CHANNEL_COUNT = 16;

    std::vector<ParamsMapping> mappingsByChannel;
    mappingsByChannel.reserve(MIDI_CHANNEL_COUNT);
    for (int channel = 0; channel < MIDI_CHANNEL_COUNT; ++channel) {
        mappingsByChannel.push_back(m_vstAudioClient->paramsMapping(SUPPORTED_CONTROLLERS, m_params.midiPort, channel));
    }

    m_sequencer.init(std::move(mappingsByChannel), m_useDynamicEvents, m_params.midiPort, m_params.midiChannel);
}

void VstSynthesiser::setMidiRouting(int port, int channel)
{
    if (!m_inited) {
        // Applied once the plugin is loaded
        m_params.midiPort = port;
        m_params.midiChannel = channel;
        return;
    }

    port = std::clamp(port, 0, std::max(static_cast<int>(m_params.midiPortNames.size()) - 1, 0));
    channel = std::clamp(channel, 0, 15);

    if (port == m_params.midiPort && channel == m_params.midiChannel) {
        return;
    }

    // Notes still playing must end on the port/channel they started on
    flushSound();

    m_params.midiPort = port;
    m_params.midiChannel = channel;

    m_vstAudioClient->setEventInputBus(port);
    initSequencer();

    // The articulation/CC state now has to reach the new channel
    if (m_sequencer.isActive()) {
        m_midiStateChasePending = true;
    }
}

void VstSynthesiser::toggleVolumeGain(const bool isActive)
{
    static constexpr muse::audio::gain_t NON_ACTIVE_GAIN = 0.5f;

    if (isActive) {
        m_vstAudioClient->setVolumeGain(m_sequencer.currentGain());
    } else {
        m_vstAudioClient->setVolumeGain(NON_ACTIVE_GAIN);
    }
}

bool VstSynthesiser::isValid() const
{
    if (!m_pluginPtr) {
        return false;
    }

    return m_pluginPtr->isLoaded();
}

muse::audio::AudioSourceType VstSynthesiser::type() const
{
    return m_params.type();
}

std::string VstSynthesiser::name() const
{
    if (!m_pluginPtr) {
        return std::string();
    }

    return m_pluginPtr->name();
}

void VstSynthesiser::flushSound()
{
    m_sequencer.flushOffstream();
    m_vstAudioClient->flushSound();
}

void VstSynthesiser::setupSound(const mpe::PlaybackSetupData& setupData)
{
    m_useDynamicEvents = setupData.supportsSingleNoteDynamics;
}

void VstSynthesiser::setupEvents(const mpe::PlaybackData& playbackData)
{
    m_sequencer.load(playbackData);
}

const mpe::PlaybackData& VstSynthesiser::playbackData() const
{
    return m_sequencer.playbackData();
}

void VstSynthesiser::setMode(const muse::audio::ProcessMode mode)
{
    if (m_mode == mode) {
        return;
    }

    AbstractSynthesizer::setMode(mode);

    bool isActive = isModePlaying(mode);
    m_sequencer.setActive(isActive);
    m_midiStateChasePending = isActive;
    // Stopping flushes every playing note anyway: a keyswitch note-off still waiting must not
    // come out later, together with the next chase's note-on of the same keyswitch
    if (!isActive) {
        m_pendingChaseOffEvents.clear();
        // A live MIDI CC that arrived after the last block must not come out at the next start, over the chase
        m_sequencer.clearLiveEvents();
    }
    toggleVolumeGain(isActive);
    m_vstAudioClient->setIsPlaying(isActive);
    m_vstAudioClient->setIsActive(isActive);

    if (mode == ProcessMode::PlayingOffline) {
        m_vstAudioClient->setProcessMode(VstProcessMode::kOffline);
    } else {
        m_vstAudioClient->setProcessMode(VstProcessMode::kRealtime);
    }
}

muse::audio::TimePosition VstSynthesiser::playbackPosition() const
{
    return m_currentPosition;
}

void VstSynthesiser::setPlaybackPosition(const muse::audio::TimePosition& position)
{
    IF_ASSERT_FAILED(position.isValid()) {
        return;
    }

    //! NOTE Don't trust that msecs_t is used everywhere here,
    // in fact, usecs_t (microseconds) is stored there.
    const usecs_t usecs = muse::secs_to_usecs(position.time());
    m_sequencer.setPlaybackPosition(msecs_t(usecs.raw()));

    m_currentPosition = position;

    if (m_sequencer.isActive()) {
        m_vstAudioClient->setVolumeGain(m_sequencer.currentGain());
        m_midiStateChasePending = true;
    }
}

void VstSynthesiser::setOutputSpec(const audio::OutputSpec& spec)
{
    m_outputSpec = spec;
    if (m_inited) {
        m_vstAudioClient->setOutputSpec(spec);
    }
}

samples_t VstSynthesiser::process(float* buffer, samples_t samplesPerChannel)
{
    if (!buffer) {
        return 0;
    }

    const msecs_t nextMsecs = samplesToMsecs(samplesPerChannel, m_outputSpec.sampleRate);
    VstSequencer::EventSequenceMap sequences = m_sequencer.movePlaybackForward(nextMsecs);
    const bool active = m_sequencer.isActive();

    if (active) {
        applyMidiStateChase(sequences);

        const VstSequencer::EventSequence liveEvents = m_sequencer.takeLiveEvents();
        if (!liveEvents.empty() && !sequences.empty()) {
            VstSequencer::EventSequence& firstSequence = sequences.begin()->second;
            firstSequence.insert(firstSequence.end(), liveEvents.cbegin(), liveEvents.cend());
        }
    }

    samples_t sampleOffset = 0;
    samples_t processedSamples = 0;

    for (auto it = sequences.cbegin(); it != sequences.cend(); ++it) {
        samples_t durationInSamples = samplesPerChannel - sampleOffset;

        auto nextIt = std::next(it);
        if (nextIt != sequences.cend()) {
            msecs_t duration = nextIt->first - it->first;
            durationInSamples = microSecsToSamples(duration, m_outputSpec.sampleRate);
        }

        IF_ASSERT_FAILED(sampleOffset + durationInSamples <= samplesPerChannel) {
            break;
        }

        processedSamples += processSequence(it->second, durationInSamples, buffer + sampleOffset * m_outputSpec.audioChannelCount);
        sampleOffset += durationInSamples;

        if (active) {
            m_currentPosition.forward(durationInSamples);
        }
    }

    return processedSamples;
}

//! NOTE: re-sends the articulation (keyswitch/CC) that should be active at the start position,
//! since playback may start mid-score, and pausing resets the plugin's CCs to their defaults.
//! Keyswitch note-offs go out on the next block, so they never arrive at the same time as their note-on
void VstSynthesiser::applyMidiStateChase(VstSequencer::EventSequenceMap& sequences)
{
    if (sequences.empty()) {
        return;
    }

    VstSequencer::EventSequence& firstSequence = sequences.begin()->second;

    if (!m_midiStateChasePending) {
        // Note-offs of the previous block's chase
        if (!m_pendingChaseOffEvents.empty()) {
            firstSequence.insert(firstSequence.begin(), m_pendingChaseOffEvents.cbegin(), m_pendingChaseOffEvents.cend());
            m_pendingChaseOffEvents.clear();
        }
        return;
    }

    m_midiStateChasePending = false;

    // A new chase (e.g. a seek right after the previous one) while note-offs are still waiting: send
    // those first, *before* the new keyswitch note-ons - off then on is a valid retrigger, the reverse
    // would cut the new keyswitch right away
    const VstSequencer::EventSequence staleOffEvents = std::move(m_pendingChaseOffEvents);
    m_pendingChaseOffEvents.clear();

    VstSequencer::EventSequence onEvents;
    m_sequencer.midiStateBefore(sequences.begin()->first, onEvents, m_pendingChaseOffEvents);
    firstSequence.insert(firstSequence.begin(), onEvents.cbegin(), onEvents.cend());
    firstSequence.insert(firstSequence.begin(), staleOffEvents.cbegin(), staleOffEvents.cend());
}

samples_t VstSynthesiser::processSequence(const VstSequencer::EventSequence& sequence, const samples_t samples, float* buffer)
{
    for (const VstSequencer::EventType& event : sequence) {
        if (std::holds_alternative<VstEvent>(event)) {
            m_vstAudioClient->handleEvent(std::get<VstEvent>(event));
        } else if (std::holds_alternative<ParamChangeEvent>(event)) {
            m_vstAudioClient->handleParamChange(std::get<ParamChangeEvent>(event));
        } else {
            muse::audio::gain_t newGain = std::get<muse::audio::gain_t>(event);
            m_vstAudioClient->setVolumeGain(newGain);
        }
    }

    if (samples == 0) {
        return 0;
    }

    return m_vstAudioClient->process(buffer, samples, m_currentPosition.samples());
}
