/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
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

#include <functional>
#include <string>

#include "modularity/imoduleinterface.h"

#include "async/channel.h"

namespace muse::audioplugins {
/**
 * @brief The plugins' work that has to run on the main thread (e.g. loading a VST3, restoring its state).
 *
 * @details Some of it blocks for seconds (e.g. a big sample player restoring its state). The tasks run one per
 * event loop turn, with a short gap in between, so that the windows can repaint between two of them (e.g. a
 * progress window) instead of all of them freezing the application at once.
 */
class IAudioPluginsMainThreadTasks : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioPluginsMainThreadTasks)

public:
    virtual ~IAudioPluginsMainThreadTasks() = default;

    using Task = std::function<void ()>;

    //! NOTE: main thread only; runs after the tasks already queued. completesPluginLoading: the plugin is loaded
    //! once this task has run (e.g. its initial state restored), see pluginLoaded()
    virtual void enqueue(const std::string& pluginName, const Task& task, bool completesPluginLoading = false) = 0;

    virtual bool isBusy() const = 0;

    //! NOTE: the plugin whose task is about to run
    virtual async::Channel<std::string> taskStarting() const = 0;
    //! NOTE: a task enqueued with completesPluginLoading has run
    virtual async::Channel<std::string> pluginLoaded() const = 0;
};
}
