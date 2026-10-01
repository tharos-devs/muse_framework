/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2022 MuseScore Limited and others
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

#include "vstsequencer.h"

#include "global/interpolation.h"

#include <algorithm>
#include <map>
#include <optional>

using namespace muse;
using namespace muse::vst;

static constexpr ControlIdx MODWHEEL_IDX = static_cast<ControlIdx>(Steinberg::Vst::kCtrlModWheel);
static constexpr ControlIdx SUSTAIN_IDX = static_cast<ControlIdx>(Steinberg::Vst::kCtrlSustainOnOff);
static constexpr ControlIdx SOSTENUTO_IDX = static_cast<ControlIdx>(Steinberg::Vst::kCtrlSustenutoOnOff);
static constexpr ControlIdx PITCH_BEND_IDX = static_cast<ControlIdx>(Steinberg::Vst::kPitchBend);

static const mpe::ArticulationTypeSet SUSTAIN_PEDAL_CC_SUPPORTED_TYPES {
    mpe::ArticulationType::Pedal,
};

static const mpe::ArticulationTypeSet SOSTENUTO_PEDAL_CC_SUPPORTED_TYPES {
    mpe::ArticulationType::LaissezVibrer,
};

//! NOTE: keyswitches are sent as short notes rather than held until the next articulation change,
//! so no keyswitch can be left hanging if playback stops in between
static constexpr mpe::timestamp_t KEYSWITCH_NOTE_DURATION_US = 10000;
static constexpr uint8_t DEFAULT_KEYSWITCH_VELOCITY = 100;
static constexpr ControlIdx PROGRAM_CHANGE_IDX = static_cast<ControlIdx>(Steinberg::Vst::kCtrlProgramChange);

static const mpe::ArticulationTypeSet BEND_SUPPORTED_TYPES {
    mpe::ArticulationType::Multibend, mpe::ArticulationType::ContinuousGlissando,
};

void VstSequencer::init(ParamsMapping&& mapping, bool useDynamicEvents)
{
    m_mapping = std::move(mapping);
    m_useDynamicEvents = useDynamicEvents;
    m_inited = true;

    updateMainStreamEvents(m_playbackData.originEvents, m_playbackData.dynamics);
}

void VstSequencer::updateMainStreamEvents(const mpe::PlaybackEventsMap& events, const mpe::DynamicAutomationLayers& dynamics)
{
    if (!m_inited) {
        return;
    }

    m_mainStreamEvents.clear();
    m_controllerStates.clear();
    m_liveOverrides.clear();

    if (m_onMainStreamFlushed) {
        m_onMainStreamFlushed();
    }

    addPlaybackEvents(m_mainStreamEvents, events, true /*recordState*/);
    sortNoteOnEventsByPitch(m_mainStreamEvents);
    addMidiMessagesEvents(m_mainStreamEvents, events, true /*recordState*/);

    if (m_useDynamicEvents) {
        addDynamicEvents(m_mainStreamEvents, dynamics);
    }

    updateMainSequenceIterator();
}

void VstSequencer::updateOffStreamEvents(const mpe::PlaybackEventsMap& events)
{
    if (isActive()) {
        mpe::PlaybackEventsMap offStreamEvents;

        for (const auto& [timestamp, eventList] : events) {
            for (const mpe::PlaybackEvent& event : eventList) {
                const auto* controllerEvent = std::get_if<mpe::ControllerChangeEvent>(&event);
                if (controllerEvent && controllerEvent->type == mpe::ControllerChangeEvent::ControlChange) {
                    addLiveControlChange(*controllerEvent);
                } else {
                    offStreamEvents[timestamp].push_back(event);
                }
            }
        }

        addPlaybackEvents(m_offStreamEvents, offStreamEvents, false /*recordState*/);
        addMidiMessagesEvents(m_offStreamEvents, offStreamEvents, false /*recordState*/);
        updateOffSequenceIterator();
        return;
    }

    addPlaybackEvents(m_offStreamEvents, events, false /*recordState*/);
    addMidiMessagesEvents(m_offStreamEvents, events, false /*recordState*/);
    updateOffSequenceIterator();
}

muse::audio::gain_t VstSequencer::currentGain() const
{
    if (!m_useDynamicEvents) {
        return 0.5f;
    }

    mpe::dynamic_level_t maxDynamicLevel = mpe::MIN_DYNAMIC_LEVEL;
    bool foundAnyLevel = false;

    for (const auto& [_, curve] : m_playbackData.dynamics) {
        if (curve.empty()) {
            continue;
        }

        foundAnyLevel = true;
        maxDynamicLevel = std::max(maxDynamicLevel, mpe::dynamicLevelFromNormalized(mpe::evaluateCurveAt(curve, m_playbackPosition)));
    }

    if (!foundAnyLevel) {
        maxDynamicLevel = mpe::dynamicLevelFromType(mpe::DynamicType::Natural);
    }

    return expressionLevel(maxDynamicLevel);
}

void VstSequencer::addPlaybackEvents(EventSequenceMap& destination, const mpe::PlaybackEventsMap& events, bool recordState)
{
    SostenutoTimeAndDurations sostenutoTimeAndDurations;

    for (const auto& evPair : events) {
        const mpe::timestamp_t notesOffset = midiMessagesNotesOffset(evPair.second);

        for (const mpe::PlaybackEvent& event : evPair.second) {
            if (std::holds_alternative<mpe::NoteEvent>(event)) {
                addNoteEvent(destination, std::get<mpe::NoteEvent>(event), notesOffset, sostenutoTimeAndDurations);
            } else if (std::holds_alternative<mpe::ControllerChangeEvent>(event)) {
                addControlChangeEvent(destination, evPair.first, std::get<mpe::ControllerChangeEvent>(event), recordState);
            }
        }
    }

    addSostenutoEvents(destination, sostenutoTimeAndDurations);
}

void VstSequencer::addDynamicEvents(EventSequenceMap& destination, const mpe::DynamicAutomationLayers& layers)
{
    constexpr mpe::timestamp_t STEP_INTERVAL_US = 30000;

    //! NOTE: VST instance has a single gain parameter, so merge layers by tracking each one's last-known level
    std::map<mpe::timestamp_t, std::vector<std::pair<mpe::layer_idx_t, mpe::dynamic_level_t> > > updatesAt;
    for (const auto& [layerIdx, curve] : layers) {
        mpe::resampleCurve(curve, STEP_INTERVAL_US, [&](mpe::timestamp_t t, muse::real_t normalized) {
            updatesAt[t].emplace_back(layerIdx, mpe::dynamicLevelFromNormalized(normalized));
        });
    }

    std::map<mpe::layer_idx_t, mpe::dynamic_level_t> currentLevel;
    std::optional<float> lastGain;

    for (const auto& [t, updates] : updatesAt) {
        for (const auto& [layerIdx, level] : updates) {
            currentLevel[layerIdx] = level;
        }

        mpe::dynamic_level_t maxDynamicLevel = mpe::MIN_DYNAMIC_LEVEL;
        for (const auto& [_, level] : currentLevel) {
            maxDynamicLevel = std::max(maxDynamicLevel, level);
        }

        const float gain = expressionLevel(maxDynamicLevel);
        if (lastGain.has_value() && muse::RealIsEqual(*lastGain, gain)) {
            continue;
        }
        lastGain = gain;
        destination[t].emplace_back(gain);
    }
}

void VstSequencer::addNoteEvent(EventSequenceMap& destination, const mpe::NoteEvent& originNoteEvent, const mpe::timestamp_t notesOffset,
                                SostenutoTimeAndDurations& sostenutoTimeAndDurations)
{
    std::optional<mpe::NoteEvent> shiftedNoteEvent;
    if (notesOffset != 0) {
        mpe::ArrangementContext shiftedCtx = originNoteEvent.arrangementCtx();
        shiftedCtx.actualTimestamp = std::max(shiftedCtx.actualTimestamp + notesOffset, mpe::timestamp_t(0));
        shiftedNoteEvent.emplace(std::move(shiftedCtx), mpe::PitchContext(originNoteEvent.pitchCtx()),
                                 mpe::ExpressionContext(originNoteEvent.expressionCtx()));
    }

    const mpe::NoteEvent& noteEvent = shiftedNoteEvent ? *shiftedNoteEvent : originNoteEvent;
    const mpe::ArrangementContext& arrangementCtx = noteEvent.arrangementCtx();
    const int32_t noteId = noteIndex(noteEvent.pitchCtx().nominalPitchLevel);
    const float velocityFraction = noteVelocityFraction(noteEvent);
    const float tuning = noteTuning(noteEvent, noteId);

    if (arrangementCtx.hasStart()) {
        if (m_useDynamicEvents) {
            destination[arrangementCtx.actualTimestamp].emplace_back(expressionLevel(noteEvent.expressionCtx().nominalDynamicLevel));
        }

        destination[arrangementCtx.actualTimestamp].emplace_back(buildEvent(VstEvent::kNoteOnEvent, noteId, velocityFraction, tuning));
    }

    if (arrangementCtx.hasEnd()) {
        const mpe::timestamp_t timestampTo = arrangementCtx.actualTimestamp + noteEvent.arrangementCtx().actualDuration;
        destination[timestampTo].emplace_back(buildEvent(VstEvent::kNoteOffEvent, noteId, velocityFraction, tuning));
    }

    for (const auto& artPair : noteEvent.expressionCtx().articulations) {
        if (artPair.first == mpe::ArticulationType::Standard) {
            continue;
        }

        const mpe::ArticulationMeta& meta = artPair.second.meta;

        if (!noteEvent.pitchCtx().pitchCurve.empty() && muse::contains(BEND_SUPPORTED_TYPES, meta.type)) {
            addPitchCurve(destination, noteEvent, meta);
            continue;
        }

        if (muse::contains(SUSTAIN_PEDAL_CC_SUPPORTED_TYPES, meta.type)) {
            addPedalEvent(destination, meta);
            continue;
        }

        if (muse::contains(SOSTENUTO_PEDAL_CC_SUPPORTED_TYPES, meta.type)) {
            const mpe::timestamp_t timestamp = arrangementCtx.actualTimestamp + noteEvent.arrangementCtx().actualDuration * 0.1; // add offset for Sostenuto to take effect
            sostenutoTimeAndDurations.push_back(mpe::TimestampAndDuration { timestamp, meta.overallDuration });
            continue;
        }
    }
}

void VstSequencer::addPedalEvent(EventSequenceMap& destination, const mpe::ArticulationMeta& meta)
{
    if (meta.hasStart()) {
        addParamChange(destination, meta.timestamp, SUSTAIN_IDX, 1);
    }

    if (meta.hasEnd()) {
        addParamChange(destination, meta.timestamp + meta.overallDuration, SUSTAIN_IDX, 0);
    }
}

void VstSequencer::addControlChangeEvent(EventSequenceMap& destination, const mpe::timestamp_t timestamp,
                                         const mpe::ControllerChangeEvent& event, bool recordState)
{
    switch (event.type) {
    case mpe::ControllerChangeEvent::Modulation:
        addParamChange(destination, timestamp, MODWHEEL_IDX, event.val);
        break;
    case mpe::ControllerChangeEvent::SustainPedalOnOff:
        addParamChange(destination, timestamp, SUSTAIN_IDX, event.val);
        break;
    case mpe::ControllerChangeEvent::PitchBend:
        addParamChange(destination, timestamp, PITCH_BEND_IDX, event.val);
        break;
    case mpe::ControllerChangeEvent::ControlChange: {
        const ControlIdx controlIdx = static_cast<ControlIdx>(event.controller);
        addParamChange(destination, timestamp, controlIdx, event.val);
        if (recordState) {
            m_controllerStates[controlIdx].insert_or_assign(timestamp, static_cast<PluginParamValue>(event.val.raw()));
        }
    } break;
    case mpe::ControllerChangeEvent::Undefined:
        break;
    }
}

void VstSequencer::addLiveControlChange(const mpe::ControllerChangeEvent& event)
{
    auto controlIt = m_mapping.find(static_cast<ControlIdx>(event.controller));
    if (controlIt == m_mapping.cend()) {
        return;
    }

    const ControlIdx controlIdx = static_cast<ControlIdx>(event.controller);
    const PluginParamId paramId = controlIt->second;
    const PluginParamValue value = static_cast<PluginParamValue>(event.val.raw());
    m_liveEvents.emplace_back(ParamChangeEvent { paramId, value });

    auto overrideIt = m_liveOverrides.find(controlIdx);
    if (overrideIt != m_liveOverrides.end() && overrideIt->second.removedFrom <= m_playbackPosition) {
        overrideIt->second.value = value; // the main stream values from here on are already gone
        return;
    }

    m_liveOverrides.insert_or_assign(controlIdx, LiveOverride { value, m_playbackPosition });

    // The values still to come from the main stream would immediately undo the live one
    for (auto it = m_mainStreamEvents.lower_bound(m_playbackPosition); it != m_mainStreamEvents.end(); ++it) {
        EventSequence& sequence = it->second;
        sequence.erase(std::remove_if(sequence.begin(), sequence.end(), [paramId](const EventType& e) {
            const auto* paramChange = std::get_if<ParamChangeEvent>(&e);
            return paramChange && paramChange->paramId == paramId;
        }), sequence.end());
    }
}

VstSequencer::EventSequence VstSequencer::takeLiveEvents()
{
    EventSequence events = std::move(m_liveEvents);
    m_liveEvents.clear();
    return events;
}

void VstSequencer::clearLiveEvents()
{
    m_liveEvents.clear();
}

void VstSequencer::addParamChange(EventSequenceMap& destination, const mpe::timestamp_t timestamp,
                                  const ControlIdx controlIdx, const PluginParamValue value)
{
    auto controlIt = m_mapping.find(controlIdx);
    if (controlIt == m_mapping.cend()) {
        return;
    }

    const PluginParamId paramId = controlIt->second;
    EventSequence& events = destination[timestamp];

    for (const EventType& e : events) {
        if (!std::holds_alternative<ParamChangeEvent>(e)) {
            continue;
        }

        const ParamChangeEvent& pce = std::get<ParamChangeEvent>(e);
        if (pce.paramId == paramId && RealIsEqual(pce.value, value)) {
            return;
        }
    }

    events.emplace_back(ParamChangeEvent { paramId, value });
}

void VstSequencer::addPitchCurve(EventSequenceMap& destination, const mpe::NoteEvent& noteEvent,
                                 const mpe::ArticulationMeta& artMeta)
{
    auto pitchBendIt = m_mapping.find(PITCH_BEND_IDX);
    if (pitchBendIt == m_mapping.cend()) {
        return;
    }

    const mpe::timestamp_t noteTimestampTo = noteEvent.arrangementCtx().actualTimestamp + noteEvent.arrangementCtx().actualDuration;
    const mpe::timestamp_t pitchBendTimestampTo = std::min(artMeta.timestamp + artMeta.overallDuration, noteTimestampTo);

    ParamChangeEvent event;
    event.paramId = pitchBendIt->second;
    event.value = 0.5f;
    destination[pitchBendTimestampTo].push_back(event);

    auto currIt = noteEvent.pitchCtx().pitchCurve.cbegin();
    auto nextIt = std::next(currIt);
    auto endIt = noteEvent.pitchCtx().pitchCurve.cend();

    float prevBendValue = -1.f;

    for (; nextIt != endIt; currIt = nextIt, nextIt = std::next(currIt)) {
        const float currValue = pitchBendLevel(currIt->second);
        const float nextValue = pitchBendLevel(nextIt->second);

        const mpe::timestamp_t currTime = artMeta.timestamp + artMeta.overallDuration * mpe::percentageToFactor(currIt->first);
        const mpe::timestamp_t nextTime = artMeta.timestamp + artMeta.overallDuration * mpe::percentageToFactor(nextIt->first);

        using namespace muse::interpolation;
        const Point currPoint { static_cast<double>(currTime), currValue };
        const Point nextPoint { static_cast<double>(nextTime), nextValue };

        //! NOTE: Increasing this number results in fewer points being interpolated
        constexpr mpe::pitch_level_t POINT_WEIGHT = mpe::PITCH_LEVEL_STEP / 25;
        size_t pointCount = std::abs(nextIt->second - currIt->second) / POINT_WEIGHT;
        pointCount = std::max(pointCount, size_t(1));

        const std::vector<Point> points = lerp(currPoint, nextPoint, pointCount);

        for (const Point& point : points) {
            const mpe::timestamp_t time = static_cast<mpe::timestamp_t>(std::round(point.x));
            const float bendValue = static_cast<float>(point.y);

            if (time < pitchBendTimestampTo && !RealIsEqual(prevBendValue, bendValue)) {
                event.value = bendValue;
                destination[time].push_back(event);
            }

            prevBendValue = bendValue;
        }
    }
}

void VstSequencer::midiStateBefore(const audio::msecs_t position, EventSequence& onEvents, EventSequence& offEvents) const
{
    auto it = m_midiStates.lower_bound(position);
    if (it != m_midiStates.cbegin()) {
        const MidiState& state = std::prev(it)->second;
        onEvents.insert(onEvents.end(), state.onEvents.cbegin(), state.onEvents.cend());
        offEvents.insert(offEvents.end(), state.offEvents.cbegin(), state.offEvents.cend());
    }

    // A controller overridden live keeps its live value (it replaces the automation until the main stream is updated)
    for (const auto& [controlIdx, liveOverride] : m_liveOverrides) {
        auto mappingIt = m_mapping.find(controlIdx);
        if (mappingIt != m_mapping.cend()) {
            onEvents.emplace_back(ParamChangeEvent { mappingIt->second, liveOverride.value });
        }
    }

    // MIDI CC automation: the value each automated controller should have at the start position
    for (const auto& [controlIdx, values] : m_controllerStates) {
        if (m_liveOverrides.find(controlIdx) != m_liveOverrides.cend()) {
            continue;
        }

        auto valueIt = values.lower_bound(position);
        if (valueIt == values.cbegin()) {
            continue;
        }

        auto mappingIt = m_mapping.find(controlIdx);
        if (mappingIt != m_mapping.cend()) {
            onEvents.emplace_back(ParamChangeEvent { mappingIt->second, std::prev(valueIt)->second });
        }
    }
}

mpe::timestamp_t VstSequencer::midiMessagesNotesOffset(const mpe::PlaybackEventList& events)
{
    for (const mpe::PlaybackEvent& event : events) {
        if (std::holds_alternative<mpe::MidiMessagesEvent>(event)) {
            return std::get<mpe::MidiMessagesEvent>(event).notesOffset;
        }
    }

    return 0;
}

//! NOTE: must run after sortNoteOnEventsByPitch(), so keyswitch notes stay in front of the notes
//! starting at the same time, whatever their pitch
//! Messages identical to the previous ones are skipped: an articulation is only (re)sent when it actually changes
void VstSequencer::addMidiMessagesEvents(EventSequenceMap& destination, const mpe::PlaybackEventsMap& events, bool recordState)
{
    if (recordState) {
        m_midiStates.clear();
    }

    const std::vector<mpe::MidiMessage>* lastMessages = nullptr;

    for (const auto& [timestamp, eventList] : events) {
        for (const mpe::PlaybackEvent& event : eventList) {
            if (!std::holds_alternative<mpe::MidiMessagesEvent>(event)) {
                continue;
            }

            const mpe::MidiMessagesEvent& midiEvent = std::get<mpe::MidiMessagesEvent>(event);
            if (lastMessages && *lastMessages == midiEvent.messages) {
                continue;
            }

            lastMessages = &midiEvent.messages;

            EventSequence onEvents;
            EventSequence offEvents;

            for (const mpe::MidiMessage& message : midiEvent.messages) {
                switch (message.type) {
                case mpe::MidiMessage::Type::Note: {
                    const uint8_t velocity = message.value > 0 ? message.value : DEFAULT_KEYSWITCH_VELOCITY;
                    const float velocityFraction = std::min(velocity, uint8_t(127)) / 127.f;
                    onEvents.emplace_back(buildEvent(VstEvent::kNoteOnEvent, message.number, velocityFraction, 0.f));
                    offEvents.emplace_back(buildEvent(VstEvent::kNoteOffEvent, message.number, velocityFraction, 0.f));
                } break;
                case mpe::MidiMessage::Type::ControlChange:
                case mpe::MidiMessage::Type::ProgramChange: {
                    const ControlIdx controlIdx = message.type == mpe::MidiMessage::Type::ControlChange
                                                  ? static_cast<ControlIdx>(message.number)
                                                  : PROGRAM_CHANGE_IDX;
                    const uint8_t value = message.type == mpe::MidiMessage::Type::ControlChange ? message.value : message.number;

                    auto controlIt = m_mapping.find(controlIdx);
                    if (controlIt == m_mapping.cend()) {
                        break;
                    }

                    onEvents.emplace_back(ParamChangeEvent { controlIt->second, std::min(value, uint8_t(127)) / 127. });
                } break;
                }
            }

            if (onEvents.empty()) {
                continue;
            }

            const mpe::timestamp_t sendAt = std::max(timestamp + midiEvent.notesOffset + midiEvent.messagesOffset, mpe::timestamp_t(0));

            EventSequence& onSequence = destination[sendAt];
            onSequence.insert(onSequence.begin(), onEvents.cbegin(), onEvents.cend());

            if (!offEvents.empty()) {
                EventSequence& offSequence = destination[sendAt + KEYSWITCH_NOTE_DURATION_US];
                offSequence.insert(offSequence.begin(), offEvents.cbegin(), offEvents.cend());
            }

            if (recordState) {
                m_midiStates.insert_or_assign(sendAt, MidiState { std::move(onEvents), std::move(offEvents) });
            }
        }
    }
}

void VstSequencer::addSostenutoEvents(EventSequenceMap& destination, const SostenutoTimeAndDurations& sostenutoTimeAndDurations)
{
    for (size_t i = 0; i < sostenutoTimeAndDurations.size(); ++i) {
        const mpe::TimestampAndDuration& currentTnD = sostenutoTimeAndDurations.at(i);
        const mpe::timestamp_t timestampTo = currentTnD.timestamp + currentTnD.duration;

        addParamChange(destination, currentTnD.timestamp, SOSTENUTO_IDX, 1);

        if (i == sostenutoTimeAndDurations.size() - 1) {
            addParamChange(destination, timestampTo, SOSTENUTO_IDX, 0);
            continue;
        }

        const mpe::TimestampAndDuration& nextTnD = sostenutoTimeAndDurations.at(i + 1);
        if (timestampTo <= nextTnD.timestamp) { // handle potential overlap
            addParamChange(destination, timestampTo, SOSTENUTO_IDX, 0);
        }
    }
}

//! Hack to make keyswitches work until we have proper UI support
//! see: https://github.com/musescore/MuseScore/issues/32150
void VstSequencer::sortNoteOnEventsByPitch(EventSequenceMap& destination)
{
    for (auto& [_, seq] : destination) {
        if (seq.size() <= 1) {
            continue;
        }

        std::stable_sort(seq.begin(), seq.end(), [](const EventType& e1, const EventType& e2) {
            if (!std::holds_alternative<VstEvent>(e1) || !std::holds_alternative<VstEvent>(e2)) {
                return false;
            }

            const VstEvent& ve1 = std::get<VstEvent>(e1);
            const VstEvent& ve2 = std::get<VstEvent>(e2);

            if (ve1.type == VstEvent::kNoteOnEvent && ve2.type == VstEvent::kNoteOnEvent) {
                return ve1.noteOn.pitch < ve2.noteOn.pitch;
            }

            return false;
        });
    }
}

VstEvent VstSequencer::buildEvent(const VstEvent::EventTypes type, const int32_t noteIdx, const float velocityFraction,
                                  const float tuning) const
{
    VstEvent result;

    result.busIndex = 0;
    result.sampleOffset = 0;
    result.ppqPosition = 0;
    result.flags = VstEvent::kIsLive;
    result.type = type;

    if (type == VstEvent::kNoteOnEvent) {
        result.noteOn.noteId = -1;
        result.noteOn.channel = 0;
        result.noteOn.pitch = noteIdx;
        result.noteOn.tuning = tuning;
        result.noteOn.velocity = velocityFraction;
    } else {
        result.noteOff.noteId = -1;
        result.noteOff.channel = 0;
        result.noteOff.pitch = noteIdx;
        result.noteOff.tuning = tuning;
        result.noteOff.velocity = velocityFraction;
    }

    return result;
}

int32_t VstSequencer::noteIndex(const mpe::pitch_level_t pitchLevel) const
{
    float stepCount = mpe::ZERO_PITCH_LEVEL_MIDI_EQUIVALENT + pitchLevel / static_cast<float>(mpe::PITCH_LEVEL_STEP);

    return std::clamp(stepCount, 0.f, 127.f);
}

float VstSequencer::noteTuning(const mpe::NoteEvent& noteEvent, const int noteIdx) const
{
    int semitonesCount = noteIdx - mpe::ZERO_PITCH_LEVEL_MIDI_EQUIVALENT;

    mpe::pitch_level_t tuningPitchLevel = noteEvent.pitchCtx().nominalPitchLevel - semitonesCount * mpe::PITCH_LEVEL_STEP;

    return (tuningPitchLevel / static_cast<float>(mpe::PITCH_LEVEL_STEP)) * 100.f;
}

float VstSequencer::noteVelocityFraction(const mpe::NoteEvent& noteEvent) const
{
    const mpe::ExpressionContext& expressionCtx = noteEvent.expressionCtx();

    if (expressionCtx.velocityOverride.has_value()) {
        return std::clamp(expressionCtx.velocityOverride.value(), 0.f, 1.f);
    }

    mpe::dynamic_level_t dynamicLevel = expressionCtx.expressionCurve.empty()
                                        ? expressionCtx.nominalDynamicLevel : expressionCtx.expressionCurve.maxAmplitudeLevel();
    return expressionLevel(dynamicLevel);
}

float VstSequencer::expressionLevel(const mpe::dynamic_level_t dynamicLevel) const
{
    static constexpr mpe::dynamic_level_t MIN_SUPPORTED_DYNAMIC_LEVEL = mpe::dynamicLevelFromType(mpe::DynamicType::ppp);
    static constexpr mpe::dynamic_level_t MAX_SUPPORTED_DYNAMIC_LEVEL = mpe::dynamicLevelFromType(mpe::DynamicType::fff);
    static constexpr mpe::dynamic_level_t AVAILABLE_RANGE = MAX_SUPPORTED_DYNAMIC_LEVEL - MIN_SUPPORTED_DYNAMIC_LEVEL;

    if (dynamicLevel <= MIN_SUPPORTED_DYNAMIC_LEVEL) {
        return (0.5f * mpe::ONE_PERCENT) / AVAILABLE_RANGE;
    }

    if (dynamicLevel >= MAX_SUPPORTED_DYNAMIC_LEVEL) {
        return 1.f;
    }

    return RealRound((dynamicLevel - MIN_SUPPORTED_DYNAMIC_LEVEL) / static_cast<float>(AVAILABLE_RANGE), 2);
}

float VstSequencer::pitchBendLevel(const mpe::pitch_level_t pitchLevel) const
{
    static constexpr float SEMITONE_RANGE = 2.f;
    static constexpr float PITCH_BEND_SEMITONE_STEP = 0.5f / SEMITONE_RANGE;

    float pitchLevelSteps = pitchLevel / static_cast<float>(mpe::PITCH_LEVEL_STEP);
    float offset = pitchLevelSteps * PITCH_BEND_SEMITONE_STEP;

    return std::clamp(0.5f + offset, 0.f, 1.f);
}
