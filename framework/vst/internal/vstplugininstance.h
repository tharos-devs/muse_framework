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

#include <atomic>
#include <functional>
#include <memory>

#include "../ivstplugininstance.h"

#include "modularity/ioc.h"
#include "async/asyncable.h"
#include "async/notification.h"
#include "async/channel.h"
#include "audio/common/iaudiothreadsecurer.h"
#include "audio/common/audiotypes.h"
#include "audioplugins/iaudiopluginsmainthreadtasks.h"

#include "../ivstmodulesrepository.h"
#include "../vsttypes.h"
#include "vstcomponenthandler.h"

namespace muse::vst {
class VstPluginProvider;
class VstPluginInstance : public IVstPluginInstance, public async::Asyncable, public std::enable_shared_from_this<VstPluginInstance>
{
    muse::GlobalInject<muse::audio::IAudioThreadSecurer> threadSecurer;
    muse::GlobalInject<IVstModulesRepository> modulesRepo;
    muse::GlobalInject<audioplugins::IAudioPluginsMainThreadTasks> mainThreadTasks;

public:
    VstPluginInstance(const muse::audio::AudioResourceId& resourceId);
    ~VstPluginInstance() override;

    //! NOTE: runs the main-thread cleanups (deactivation, module release) of destroyed instances that haven't run yet
    //! - at shutdown the main event loop is gone, so they'd never run otherwise and the modules would never be
    //! unloaded properly (some plugins, e.g. Kontakt, then crash in their own static teardown at exit)
    static void runPendingCleanups();

    const muse::audio::AudioResourceId& resourceId() const override;
    const std::string& name() const override;
    VstPluginInstanceId id() const override;

    PluginViewPtr createView() const override;

    PluginControllerPtr controller() const override;
    PluginComponentPtr component() const override;
    PluginMidiMappingPtr midiMapping() const override;

    void updatePluginConfig(const muse::audio::AudioUnitConfig& config) override;
    void refreshConfig() override;
    std::optional<muse::audio::AudioUnitConfig> refreshConfigNow() override;

    void load();

    bool isLoaded() const override;

    async::Notification loadingCompleted() const override;

    async::Channel<muse::audio::AudioUnitConfig> pluginSettingsChanged() const override;

private:
    void doLoad();
    //! NOTE: runs the main thread work through IAudioPluginsMainThreadTasks (one task per event loop turn), and
    //! only while this instance still exists: it's kept alive while the work runs
    void runOnMainThread(const std::function<void(VstPluginInstance*)>& work, bool completesLoading = false);
    void syncControllerToComponentState();
    std::optional<muse::audio::AudioUnitConfig> rescanParams();
    void setPluginConfig(const muse::audio::AudioUnitConfig& config);

    VstPluginInstanceId m_id = 0;
    muse::audio::AudioResourceId m_resourceId;

    PluginModulePtr m_module = nullptr;
    std::unique_ptr<VstPluginProvider> m_pluginProvider;

    Steinberg::FUnknownPtr<VstComponentHandler> m_componentHandlerPtr = nullptr;

    VstMemoryStream m_componentStateBuffer;
    VstMemoryStream m_controllerStateBuffer;
    mutable async::Channel<muse::audio::AudioUnitConfig> m_pluginSettingsChanges;
    std::atomic_bool m_rescanQueued = false;

    std::atomic_bool m_isLoaded = false;
    //! NOTE: the first config (the saved state, applied right after loading, see VstSynthesiser/VstFxProcessor)
    //! completes the loading
    std::atomic_bool m_initialConfigQueued = false;
    std::atomic_bool m_initialConfigApplied = false;
    async::Notification m_loadingCompleted;
};
}
