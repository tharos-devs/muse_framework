/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include <optional>

#include "async/channel.h"
#include "modularity/imoduleinterface.h"
#include "audio/common/audiotypes.h"

#include "vsttypes.h"

namespace muse::vst {
//! NOTE: where an instance plays, which a plugin replacing it takes over: a track's instrument, or an effect slot of a
//! track (trackId -1: the master's)
struct VstPluginSlot {
    bool isInstrument = false;
    muse::audio::TrackId trackId = -1;
    muse::audio::AudioFxChainOrder chainOrder = 0;

    bool operator==(const VstPluginSlot& other) const = default;
};

class IVstInstancesRegister : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IVstInstancesRegister)
public:
    virtual ~IVstInstancesRegister() = default;

    // make
    virtual IVstPluginInstancePtr makeAndRegisterInstrPlugin(const muse::audio::AudioResourceId& resourceId,
                                                             const muse::audio::TrackId trackId) = 0;

    virtual IVstPluginInstancePtr makeAndRegisterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                          const muse::audio::TrackId trackId,
                                                          const muse::audio::AudioFxChainOrder chainOrder) = 0;

    virtual IVstPluginInstancePtr makeAndRegisterMasterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                                const muse::audio::AudioFxChainOrder chainOrder) = 0;
    // register
    virtual void registerInstrPlugin(const muse::audio::TrackId trackId, IVstPluginInstancePtr instance) = 0;

    virtual void registerFxPlugin(const muse::audio::TrackId trackId, const muse::audio::AudioFxChainOrder chainOrder,
                                  IVstPluginInstancePtr instance) = 0;

    virtual void registerMasterFxPlugin(const muse::audio::AudioFxChainOrder chainOrder, IVstPluginInstancePtr instance) = 0;

    // get
    virtual IVstPluginInstancePtr instanceById(const VstPluginInstanceId id) const = 0;

    virtual IVstPluginInstancePtr instrumentPlugin(const muse::audio::AudioResourceId& resourceId,
                                                   const muse::audio::TrackId trackId) const = 0;

    virtual IVstPluginInstancePtr fxPlugin(const muse::audio::AudioResourceId& resourceId, const muse::audio::TrackId trackId,
                                           const muse::audio::AudioFxChainOrder chainOrder) const = 0;

    virtual IVstPluginInstancePtr masterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                 const muse::audio::AudioFxChainOrder chainOrder) const = 0;

    // unregister
    virtual void unregisterById(const VstPluginInstanceId id) = 0;

    virtual void unregisterInstrPlugin(const muse::audio::AudioResourceId& resourceId, const muse::audio::TrackId trackId) = 0;

    virtual void unregisterFxPlugin(const muse::audio::AudioResourceId& resourceId, const muse::audio::TrackId trackId,
                                    const muse::audio::AudioFxChainOrder chainOrder) = 0;

    virtual void unregisterMasterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                          const muse::audio::AudioFxChainOrder chainOrder) = 0;

    virtual void unregisterAllInstrPlugin() = 0;
    virtual void unregisterAllFx() = 0;

    //! NOTE: whether an instance's window is open (main thread), see VstActionsController::editorOperation()
    virtual void setEditorOpened(const VstPluginInstanceId id, bool opened) = 0;

    //! NOTE: sent from the thread registering/unregistering (the audio engine's), e.g. for the instances' windows to
    //! follow (see VstActionsController::onInstanceUnregistered()). clearingAll: all the instruments or all the
    //! effects removed at once (e.g. the project is closed), nothing replaces them
    virtual async::Channel<VstPluginInstanceId, VstPluginSlot> instanceRegistered() const = 0;
    virtual async::Channel<VstPluginInstanceId, VstPluginSlot, bool /*clearingAll*/> instanceUnregistered() const = 0;
    virtual std::optional<VstPluginInstanceId> instanceIdAt(const VstPluginSlot& slot) const = 0;
};
}
