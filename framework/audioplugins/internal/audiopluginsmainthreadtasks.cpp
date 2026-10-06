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

#include "audiopluginsmainthreadtasks.h"

#include <algorithm>

#include <QTimer>

#include "log.h"

using namespace muse::audioplugins;

//! NOTE: lets the windows repaint between two tasks (about a frame)
static constexpr int TASK_GAP_MS = 16;

void AudioPluginsMainThreadTasks::enqueue(const std::string& pluginName, const Task& task, bool completesPluginLoading)
{
    //! NOTE: a plugin's loading completes before others start loading: e.g. its saved state is restored right
    //! after it was loaded, not once all the other plugins are loaded
    if (completesPluginLoading) {
        const auto firstNotCompleting = std::find_if(m_tasks.begin(), m_tasks.end(), [](const QueuedTask& queued) {
            return !queued.completesPluginLoading;
        });
        m_tasks.insert(firstNotCompleting, { pluginName, task, completesPluginLoading });
    } else {
        m_tasks.push_back({ pluginName, task, completesPluginLoading });
    }

    //! NOTE: queued from a task, it runs after it (see scheduleNext())
    if (!m_scheduled) {
        scheduleNext();
    }
}

bool AudioPluginsMainThreadTasks::isBusy() const
{
    return m_scheduled;
}

void AudioPluginsMainThreadTasks::scheduleNext()
{
    if (m_tasks.empty()) {
        m_scheduled = false;
        return;
    }

    m_scheduled = true;
    m_taskStarting.send(m_tasks.front().pluginName);

    QTimer::singleShot(TASK_GAP_MS, &m_timerContext, [this]() {
        const QueuedTask queued = std::move(m_tasks.front());
        m_tasks.pop_front();

        //! NOTE: whatever happens, the next tasks still run
        try {
            queued.task();
        } catch (...) {
            LOGE() << "a main thread task of the plugin failed: " << queued.pluginName;
        }

        if (queued.completesPluginLoading) {
            m_pluginLoaded.send(queued.pluginName);
        }

        scheduleNext();
    });
}

muse::async::Channel<std::string> AudioPluginsMainThreadTasks::taskStarting() const
{
    return m_taskStarting;
}

muse::async::Channel<std::string> AudioPluginsMainThreadTasks::pluginLoaded() const
{
    return m_pluginLoaded;
}
