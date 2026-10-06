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

#include "vstinstancesregister.h"

#include "global/containers.h"

#include "vstplugininstance.h"

#include "log.h"

using namespace muse::vst;
using namespace muse::audio;

IVstPluginInstancePtr VstInstancesRegister::makeAndRegisterInstrPlugin(const AudioResourceId& resourceId,
                                                                       const muse::audio::TrackId trackId)
{
    std::shared_ptr<VstPluginInstance> instance = std::make_shared<VstPluginInstance>(resourceId);

    registerInstrPlugin(trackId, instance);

    instance->load();

    return instance;
}

IVstPluginInstancePtr VstInstancesRegister::makeAndRegisterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                                    const muse::audio::TrackId trackId,
                                                                    const muse::audio::AudioFxChainOrder chainOrder)
{
    std::shared_ptr<VstPluginInstance> instance = std::make_shared<VstPluginInstance>(resourceId);

    registerFxPlugin(trackId, chainOrder, instance);

    instance->load();

    return instance;
}

IVstPluginInstancePtr VstInstancesRegister::makeAndRegisterMasterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                                          const muse::audio::AudioFxChainOrder chainOrder)
{
    std::shared_ptr<VstPluginInstance> instance = std::make_shared<VstPluginInstance>(resourceId);

    registerMasterFxPlugin(chainOrder, instance);

    instance->load();

    return instance;
}

VstPluginSlot VstInstancesRegister::slotOf(const Key& key)
{
    return VstPluginSlot { key.type == Type::Instrument, key.trackId, key.chainOrder };
}

//! NOTE: the notifications are sent without holding the lock
void VstInstancesRegister::registerPlugin(const Key& key, IVstPluginInstancePtr instance)
{
    IF_ASSERT_FAILED(instance) {
        return;
    }

    std::optional<VstPluginInstanceId> replacedId;
    {
        std::lock_guard lock(m_mutex);

        auto it = m_instances.find(key);
        if (it != m_instances.end() && it->second->id() != instance->id()) {
            replacedId = it->second->id();
        }

        m_instances.insert_or_assign(key, instance);
    }

    if (replacedId) {
        m_instanceUnregistered.send(*replacedId, slotOf(key), false);
    }

    m_instanceRegistered.send(instance->id(), slotOf(key));
}

std::vector<VstInstancesRegister::SlotInstance> VstInstancesRegister::takeInstances(
    const std::function<bool(const Key&, const IVstPluginInstancePtr&)>& pred)
{
    std::vector<SlotInstance> removed;

    std::lock_guard lock(m_mutex);
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        if (pred(it->first, it->second)) {
            removed.emplace_back(it->second->id(), slotOf(it->first));
            it = m_instances.erase(it);
        } else {
            ++it;
        }
    }

    return removed;
}

void VstInstancesRegister::notifyUnregistered(const std::vector<SlotInstance>& removed, bool clearingAll)
{
    for (const SlotInstance& instance : removed) {
        m_instanceUnregistered.send(instance.first, instance.second, clearingAll);
    }
}

async::Channel<VstPluginInstanceId, VstPluginSlot> VstInstancesRegister::instanceRegistered() const
{
    return m_instanceRegistered;
}

async::Channel<VstPluginInstanceId, VstPluginSlot, bool> VstInstancesRegister::instanceUnregistered() const
{
    return m_instanceUnregistered;
}

std::optional<VstPluginInstanceId> VstInstancesRegister::instanceIdAt(const VstPluginSlot& slot) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& [key, instance] : m_instances) {
        if (slotOf(key) == slot) {
            return instance->id();
        }
    }

    return std::nullopt;
}

void VstInstancesRegister::registerInstrPlugin(const muse::audio::TrackId trackId, IVstPluginInstancePtr instance)
{
    IF_ASSERT_FAILED(instance) {
        return;
    }

    registerPlugin({ Type::Instrument, instance->resourceId(), trackId, 0 }, instance);
}

void VstInstancesRegister::registerFxPlugin(const muse::audio::TrackId trackId,
                                            const AudioFxChainOrder chainOrder,
                                            IVstPluginInstancePtr instance)
{
    IF_ASSERT_FAILED(instance) {
        return;
    }

    registerPlugin({ Type::Effect, instance->resourceId(), trackId, chainOrder }, instance);
}

void VstInstancesRegister::registerMasterFxPlugin(const AudioFxChainOrder chainOrder, IVstPluginInstancePtr instance)
{
    registerFxPlugin(-1, chainOrder, instance);
}

IVstPluginInstancePtr VstInstancesRegister::instanceById(const VstPluginInstanceId id) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& p : m_instances) {
        if (p.second->id() == id) {
            return p.second;
        }
    }
    return nullptr;
}

static std::optional<AudioUnitConfig> stateOf(const IVstPluginInstancePtr& instance)
{
    if (!instance || !instance->isLoaded()) {
        return std::nullopt;
    }

    return instance->refreshConfigNow();
}

std::optional<AudioUnitConfig> VstInstancesRegister::instrumentPluginState(const AudioResourceId& resourceId, const TrackId trackId) const
{
    return stateOf(instrumentPlugin(resourceId, trackId));
}

std::optional<AudioUnitConfig> VstInstancesRegister::fxPluginState(const AudioResourceId& resourceId, const TrackId trackId,
                                                                   const AudioFxChainOrder chainOrder) const
{
    return stateOf(fxPlugin(resourceId, trackId, chainOrder));
}

std::optional<AudioUnitConfig> VstInstancesRegister::masterFxPluginState(const AudioResourceId& resourceId,
                                                                         const AudioFxChainOrder chainOrder) const
{
    return stateOf(masterFxPlugin(resourceId, chainOrder));
}

//! NOTE: not through instrumentPlugin(): asked for tracks that have no such plugin, which it logs as an error
bool VstInstancesRegister::isInstrumentEditorOpened(const AudioResourceId& resourceId, const TrackId trackId) const
{
    VstPluginInstanceId id = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_instances.find({ Type::Instrument, resourceId, trackId, 0 });
        if (it == m_instances.end()) {
            return false;
        }
        id = it->second->id();
    }

    return muse::contains(m_openedEditors, id);
}

async::Notification VstInstancesRegister::editorsOpenedChanged() const
{
    return m_editorsOpenedChanged;
}

void VstInstancesRegister::setEditorOpened(const VstPluginInstanceId id, bool opened)
{
    const bool changed = opened ? m_openedEditors.insert(id).second : m_openedEditors.erase(id) > 0;
    if (changed) {
        m_editorsOpenedChanged.notify();
    }
}

IVstPluginInstancePtr VstInstancesRegister::instrumentPlugin(const muse::audio::AudioResourceId& resourceId,
                                                             const muse::audio::TrackId trackId) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_instances.find({ Type::Instrument, resourceId, trackId, 0 });
    if (it != m_instances.end()) {
        return it->second;
    }

    LOGE() << "Unable to find instrument plugin, trackId: " << trackId
           << " , resourceId: " << resourceId;

    return nullptr;
}

IVstPluginInstancePtr VstInstancesRegister::fxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                     const muse::audio::TrackId trackId,
                                                     const AudioFxChainOrder chainOrder) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_instances.find({ Type::Effect, resourceId, trackId, chainOrder });
    if (it != m_instances.end()) {
        return it->second;
    }

    LOGE() << "Unable to find fx plugin, trackId: " << trackId
           << ", resourceId: " << resourceId
           << ", chainOrder: " << chainOrder;

    return nullptr;
}

IVstPluginInstancePtr VstInstancesRegister::masterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                                           const AudioFxChainOrder chainOrder) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_instances.find({ Type::Effect, resourceId, -1, chainOrder });
    if (it != m_instances.end()) {
        return it->second;
    }

    LOGE() << "Unable to find master fx plugin"
           << ", resourceId: " << resourceId
           << ", chainOrder: " << chainOrder;

    return nullptr;
}

void VstInstancesRegister::unregisterById(const VstPluginInstanceId id)
{
    notifyUnregistered(takeInstances([id](const Key&, const IVstPluginInstancePtr& instance) {
        return instance->id() == id;
    }), false);
}

void VstInstancesRegister::unregisterInstrPlugin(const muse::audio::AudioResourceId& resourceId, const muse::audio::TrackId trackId)
{
    const Key removedKey { Type::Instrument, resourceId, trackId, 0 };
    notifyUnregistered(takeInstances([&removedKey](const Key& key, const IVstPluginInstancePtr&) {
        return key == removedKey;
    }), false);
}

void VstInstancesRegister::unregisterFxPlugin(const muse::audio::AudioResourceId& resourceId,
                                              const muse::audio::TrackId trackId,
                                              const AudioFxChainOrder chainOrder)
{
    const Key removedKey { Type::Effect, resourceId, trackId, chainOrder };
    notifyUnregistered(takeInstances([&removedKey](const Key& key, const IVstPluginInstancePtr&) {
        return key == removedKey;
    }), false);
}

void VstInstancesRegister::unregisterMasterFxPlugin(const muse::audio::AudioResourceId& resourceId, const AudioFxChainOrder chainOrder)
{
    unregisterFxPlugin(resourceId, -1, chainOrder);
}

void VstInstancesRegister::unregisterAllInstrPlugin()
{
    notifyUnregistered(takeInstances([](const Key& key, const IVstPluginInstancePtr&) {
        return key.type == Type::Instrument;
    }), true);
}

void VstInstancesRegister::unregisterAllFx()
{
    notifyUnregistered(takeInstances([](const Key& key, const IVstPluginInstancePtr&) {
        return key.type == Type::Effect;
    }), true);
}
