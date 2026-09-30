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

#include <functional>
#include <vector>

#include "global/async/asyncable.h"

#include "global/modularity/ioc.h"
#include "audio/common/rpc/irpcchannel.h"

#include "audio/common/audiotypes.h"
#include "../nodes/audionode.h"

#include "abstractaudioencoder.h"

namespace muse::io {
class IODevice;
}

namespace muse::audio::soundtrack {
class SoundTrackWriter : public async::Asyncable
{
    muse::GlobalInject<rpc::IRpcChannel> rpcChannel;

public:
    SoundTrackWriter(io::IODevice& dstDevice, const SoundTrackFormat& format, const secs_t totalDuration, IAudioNodePtr source);

    //! NOTE The first `duration` of the rendered audio is a pre-roll: `handler(true)` is called before
    //! rendering it, `handler(false)` right after (see SoundTrackFormat::startTime)
    using PreRollHandler = std::function<void (bool preRoll)>;
    void setPreRoll(const secs_t duration, const PreRollHandler& handler);

    Ret write();
    void abort();

    Progress progress();

private:
    Ret writeStreaming();

    void sendProgress(uint64_t framesWritten, uint64_t totalFrames);

    IAudioNodePtr m_source;

    std::vector<float> m_intermBuffer;
    samples_t m_renderStep = 0;
    samples_t m_leadingSilenceSamples = 0;
    samples_t m_dataSamples = 0;
    samples_t m_totalSamples = 0;
    samples_t m_preRollSamples = 0;
    PreRollHandler m_preRollHandler;

    encode::AbstractAudioEncoderPtr m_encoderPtr;

    Progress m_progress;
    std::atomic<bool> m_isAborted = false;
};
}
