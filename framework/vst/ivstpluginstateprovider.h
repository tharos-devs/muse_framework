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

#include "async/notification.h"
#include "modularity/imoduleinterface.h"
#include "audio/common/audiotypes.h"

namespace muse::vst {
//! NOTE: the state of a VST plugin, read from it right now (main thread). Some plugins change their state without
//! reporting it (e.g. Kontakt's solo/mute): what was last reported can be outdated, e.g. when saving a project
class IVstPluginStateProvider : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IVstPluginStateProvider)
public:
    virtual ~IVstPluginStateProvider() = default;

    virtual std::optional<muse::audio::AudioUnitConfig> instrumentPluginState(const muse::audio::AudioResourceId& resourceId,
                                                                              const muse::audio::TrackId trackId) const = 0;
    virtual std::optional<muse::audio::AudioUnitConfig> fxPluginState(const muse::audio::AudioResourceId& resourceId,
                                                                      const muse::audio::TrackId trackId,
                                                                      const muse::audio::AudioFxChainOrder chainOrder) const = 0;
    virtual std::optional<muse::audio::AudioUnitConfig> masterFxPluginState(const muse::audio::AudioResourceId& resourceId,
                                                                            const muse::audio::AudioFxChainOrder chainOrder) const = 0;

    //! NOTE: whether the instrument plugin's window is open (main thread)
    virtual bool isInstrumentEditorOpened(const muse::audio::AudioResourceId& resourceId, const muse::audio::TrackId trackId) const = 0;
    virtual async::Notification editorsOpenedChanged() const = 0;
};
}
