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

#pragma once

#include "audio/engine/internal/abstracteventsequencer.h"

#include "vsttypes.h"

namespace muse::vst {
class VstSequencer : public audio::engine::AbstractEventSequencer<VstEvent, ParamChangeEvent, muse::audio::gain_t>
{
public:
    void init(ParamsMapping&& mapping, bool useDynamicEvents);

    muse::audio::gain_t currentGain() const;

    //! NOTE: the raw MIDI state (keyswitch, CC, program change) last sent strictly before the given position,
    //! to re-send when playback starts mid-score or resumes after a pause (flushSound() resets CCs to their defaults)
    //! Keyswitch note-offs are returned separately, so they can be sent a little later than their note-ons
    void midiStateBefore(const audio::msecs_t position, EventSequence& onEvents, EventSequence& offEvents) const;

    //! NOTE: notes and MIDI CCs received live (off-stream) while playing, to send right away - the off stream itself
    //! is only played when stopped. Each CC also overrides its controller's main stream values from there
    //! on (a CC being recorded replaces the existing curve), until the main stream is updated again
    EventSequence takeLiveEvents();
    void clearLiveEvents();

private:
    void updateMainStreamEvents(const mpe::PlaybackEventsMap& events, const mpe::DynamicAutomationLayers& dynamics) override;
    void updateOffStreamEvents(const mpe::PlaybackEventsMap& events) override;

    using SostenutoTimeAndDurations = std::vector<mpe::TimestampAndDuration>;

    void addPlaybackEvents(EventSequenceMap& destination, const mpe::PlaybackEventsMap& events, bool recordState);
    void addDynamicEvents(EventSequenceMap& destination, const mpe::DynamicAutomationLayers& layers);
    void addNoteEvent(EventSequenceMap& destination, const mpe::NoteEvent& noteEvent, const mpe::timestamp_t notesOffset,
                      SostenutoTimeAndDurations& sostenutoTimeAndDurations);
    void addPedalEvent(EventSequenceMap& destination, const mpe::ArticulationMeta& meta);
    void addControlChangeEvent(EventSequenceMap& destination, const mpe::timestamp_t timestamp, const mpe::ControllerChangeEvent& event,
                               bool recordState);
    void addLiveControlChange(const mpe::ControllerChangeEvent& event);
    void addLiveNoteAndControllerEvents(const mpe::PlaybackEventsMap& events);
    void addParamChange(EventSequenceMap& destination, const mpe::timestamp_t timestamp, const ControlIdx controlIdx,
                        const PluginParamValue value);
    void addPitchCurve(EventSequenceMap& destination, const mpe::NoteEvent& noteEvent, const mpe::ArticulationMeta& artMeta);
    static mpe::timestamp_t midiMessagesNotesOffset(const mpe::PlaybackEventList& events);
    void addMidiMessagesEvents(EventSequenceMap& destination, const mpe::PlaybackEventsMap& events, bool recordState);
    void addSostenutoEvents(EventSequenceMap& destination, const SostenutoTimeAndDurations& sostenutoTimeAndDurations);

    void sortNoteOnEventsByPitch(EventSequenceMap& destination);

    VstEvent buildEvent(const Steinberg::Vst::Event::EventTypes type, const int32_t noteIdx, const float velocityFraction,
                        const float tuning) const;

    int32_t noteIndex(const mpe::pitch_level_t pitchLevel) const;
    float noteTuning(const mpe::NoteEvent& noteEvent, const int noteIdx) const;
    float noteVelocityFraction(const mpe::NoteEvent& noteEvent) const;
    float expressionLevel(const mpe::dynamic_level_t dynamicLevel) const;
    float pitchBendLevel(const mpe::pitch_level_t pitchLevel) const;

    bool m_inited = false;
    bool m_useDynamicEvents = false;
    ParamsMapping m_mapping;

    struct MidiState {
        EventSequence onEvents;
        EventSequence offEvents;
    };

    std::map<audio::msecs_t, MidiState> m_midiStates;

    //! NOTE: MIDI CC automation values sent over time, per controller (main stream only)
    std::map<ControlIdx, std::map<audio::msecs_t, PluginParamValue> > m_controllerStates;

    EventSequence m_liveEvents;

    //! NOTE: per controller overridden live since the main stream was last updated: its latest live value
    //! (what a chase after a seek must restore, not the overridden automation) and the position from which
    //! its main stream values were already removed (so that a fast controller doesn't rescan the stream
    //! on every message)
    struct LiveOverride {
        PluginParamValue value = 0.;
        audio::msecs_t removedFrom = 0;
    };
    std::map<ControlIdx, LiveOverride> m_liveOverrides;
};
}
