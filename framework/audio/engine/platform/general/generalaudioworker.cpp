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
#include "generalaudioworker.h"

#include <chrono>
#include <future>

#include "global/concurrency/threadutils.h"

#ifdef Q_OS_WIN
#include "global/platform/win/waitabletimer.h"
#endif

#include "audio/common/audiosanitizer.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

static uint64_t toWinTime(const msecs_t msecs)
{
    return msecs.raw() * 10000;
}

GeneralAudioWorker::GeneralAudioWorker()
{
}

GeneralAudioWorker::~GeneralAudioWorker()
{
    if (m_running) {
        stop();
    }
}

void GeneralAudioWorker::run(Callback callback)
{
    m_thread = std::make_unique<std::thread>([this, callback]() {
        th_main(callback);
    });

    if (!muse::setThreadPriority(*m_thread, ThreadPriority::High)) {
        LOGE() << "Unable to change audio thread priority";
    }
}

void GeneralAudioWorker::setInterval(const msecs_t interval)
{
    ONLY_AUDIO_ENGINE_THREAD;

    m_intervalMsecs = interval;
    m_intervalInWinTime = toWinTime(interval);
}

static msecs_t audioWorkerInterval(const samples_t samples, const sample_rate_t sampleRate)
{
    msecs_t interval = float(samples) / 4.f / float(sampleRate) * 1000.f;
    interval = std::max(interval, msecs_t(1));

    // Found experementaly on a slow laptop (2 core) running on battery power
    interval = std::min(interval, msecs_t(10));

    return interval;
}

void GeneralAudioWorker::setInterval(const samples_t samples, const sample_rate_t sampleRate)
{
    setInterval(audioWorkerInterval(samples, sampleRate));
}

void GeneralAudioWorker::stop()
{
    m_running = false;

    if (!m_thread) {
        return;
    }

    //! NOTE: std::thread has no built-in timed join. If th_main is genuinely stuck inside
    //! a blocking call (e.g. a misbehaving VST3 plugin during teardown), an untimed join()
    //! here would hang app shutdown forever. Hand the thread off to a detached watcher that
    //! joins it whenever it actually finishes (even well after this function returns), and
    //! only wait here for a bounded time before giving up and letting shutdown continue.
    auto donePromise = std::make_shared<std::promise<void> >();
    std::future<void> done = donePromise->get_future();

    std::thread watcher([owned = std::move(m_thread), donePromise]() mutable {
        owned->join();
        donePromise->set_value();
    });
    watcher.detach();

    constexpr auto STOP_TIMEOUT = std::chrono::seconds(3);
    if (done.wait_for(STOP_TIMEOUT) != std::future_status::ready) {
        LOGW() << "audio worker thread did not stop within " << STOP_TIMEOUT.count()
               << "s; abandoning it in the background rather than hang shutdown";
    }
}

bool GeneralAudioWorker::isRunning() const
{
    return m_running;
}

void GeneralAudioWorker::th_main(Callback callback)
{
    m_intervalMsecs = 1;
    m_intervalInWinTime = toWinTime(m_intervalMsecs);

    m_running = true;

#ifdef Q_OS_WIN
    WaitableTimer timer;
    bool timerValid = timer.init();
    if (timerValid) {
        LOGI() << "Waitable timer successfully created, interval: " << m_intervalMsecs << " ms";
    }
#endif

    while (m_running) {
        callback();

#ifdef Q_OS_WIN
        if (!timerValid || !timer.setAndWait(m_intervalInWinTime)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_intervalMsecs));
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(m_intervalMsecs));
#endif
    }
}
