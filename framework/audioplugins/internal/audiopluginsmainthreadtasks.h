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

#include <deque>

#include <QObject>

#include "../iaudiopluginsmainthreadtasks.h"

namespace muse::audioplugins {
class AudioPluginsMainThreadTasks : public IAudioPluginsMainThreadTasks
{
public:
    void enqueue(const std::string& pluginName, const Task& task, bool completesPluginLoading = false) override;

    bool isBusy() const override;

    async::Channel<std::string> taskStarting() const override;
    async::Channel<std::string> pluginLoaded() const override;

private:
    struct QueuedTask {
        std::string pluginName;
        Task task;
        bool completesPluginLoading = false;
    };

    void scheduleNext();

    std::deque<QueuedTask> m_tasks;
    bool m_scheduled = false;
    //! NOTE: the gap's timer doesn't fire once this is destroyed
    QObject m_timerContext;

    async::Channel<std::string> m_taskStarting;
    async::Channel<std::string> m_pluginLoaded;
};
}
