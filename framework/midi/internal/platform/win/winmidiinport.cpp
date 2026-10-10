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
#include "winmidiinport.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#ifdef ERROR
#undef ERROR
#endif

#include "midierrors.h"
#include "translation.h"
#include "defer.h"
#include "log.h"

struct muse::midi::WinMidiInPort::Win {
    struct Input {
        HMIDIIN midiIn = nullptr;
        MIDIHDR header {};
        std::vector<uint8_t> buffer;
    };

    // the opened devices: one, or every device with ALL_DEVICES_ID (MIDIHDR must not move once prepared)
    std::vector<std::unique_ptr<Input> > inputs;
};

using namespace muse;
using namespace muse::midi;

namespace wmidi_prv {
static std::string errorString(MMRESULT ret)
{
    switch (ret) {
    case MMSYSERR_NOERROR: return "MMSYSERR_NOERROR";
    case MIDIERR_NODEVICE: return "MIDIERR_NODEVICE";
    case MMSYSERR_ALLOCATED: return "MMSYSERR_ALLOCATED";
    case MMSYSERR_BADDEVICEID: return "MMSYSERR_BADDEVICEID";
    case MMSYSERR_INVALPARAM: return "MMSYSERR_INVALPARAM";
    case MMSYSERR_NOMEM: return "MMSYSERR_NOMEM";
    }

    return "UNKNOWN";
}
}

void WinMidiInPort::init()
{
    m_win = std::make_shared<Win>();

    m_devicesListener.startWithCallback([this]() {
        return availableDevices();
    });

    m_devicesListener.devicesChanged().onNotify(this, [this]() {
        //! NOTE: a device id holds the device's index, which changes when another device is unplugged:
        //! every device is opened again
        if (m_deviceID == ALL_DEVICES_ID) {
            connect(ALL_DEVICES_ID);
            m_availableDevicesChanged.notify();
            return;
        }

        bool connectedDeviceRemoved = true;
        for (const MidiDevice& device: availableDevices()) {
            if (m_deviceID == device.id) {
                connectedDeviceRemoved = false;
            }
        }

        if (connectedDeviceRemoved) {
            disconnect();
        }

        m_availableDevicesChanged.notify();
    });
}

void WinMidiInPort::deinit()
{
    if (isConnected()) {
        disconnect();
    }
}

MidiDeviceList WinMidiInPort::availableDevices() const
{
    std::lock_guard lock(m_devicesMutex);
    MidiDeviceList ret;

    ret.push_back({ NONE_DEVICE_ID, muse::trc("midi", "No device") });
    ret.push_back({ ALL_DEVICES_ID, muse::trc("midi", "All devices") });

    unsigned int numDevs = midiInGetNumDevs();
    if (numDevs == 0) {
        return ret;
    }

    for (unsigned int i = 0; i < numDevs; i++) {
        MIDIINCAPSW devCaps;
        midiInGetDevCapsW(i, &devCaps, sizeof(MIDIINCAPSW));

        std::wstring wstr(devCaps.szPname);
        std::string str(wstr.begin(), wstr.end());

        MidiDevice dev;
        dev.id = makeUniqueDeviceId(i, devCaps.wMid, devCaps.wPid);
        dev.name = str;

        ret.push_back(std::move(dev));
    }

    return ret;
}

async::Notification WinMidiInPort::availableDevicesChanged() const
{
    return m_availableDevicesChanged;
}

static void CALLBACK process(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    WinMidiInPort* self = reinterpret_cast<WinMidiInPort*>(dwInstance);

    switch (wMsg) {
    case MIM_OPEN:
    case MIM_CLOSE:
        break;
    case MIM_DATA:
        self->doProcess(static_cast<uint32_t>(dwParam1), static_cast<tick_t>(dwParam2));
        break;
    case MIM_LONGDATA: {
        MIDIHDR* hdr = reinterpret_cast<MIDIHDR*>(dwParam1);
        if (hdr->dwBytesRecorded > 0) {
            uint8_t* data = reinterpret_cast<uint8_t*>(hdr->lpData);
            self->doProcessLongData(data, hdr->dwBytesRecorded, static_cast<tick_t>(dwParam2));
            // while closing, midiInReset() returns the buffer: added again, it couldn't be unprepared
            if (!self->isClosing()) {
                midiInAddBuffer(hMidiIn, hdr, sizeof(MIDIHDR));
            }
        }
    } break;
    default:
        NOT_IMPLEMENTED << wMsg;
    }
}

//! NOTE: every opened device calls back from its own thread
void WinMidiInPort::doProcess(uint32_t message, tick_t tick)
{
    auto e = Event::fromMidi10Package(message).toMIDI20();
    if (e) {
        std::lock_guard lock(m_processMutex);
        m_eventReceived.send(tick, e);
    }
}

void WinMidiInPort::doProcessLongData(uint8_t* data, size_t size, tick_t tick)
{
    std::vector<Event> events = Event::fromMidi10SysExBytes(data, size);
    std::lock_guard lock(m_processMutex);
    for (const Event& e : events) {
        m_eventReceived.send(tick, e);
    }
}

Ret WinMidiInPort::connect(const MidiDeviceID& deviceID)
{
    DEFER {
        m_deviceChanged.notify();
    };

    if (isConnected()) {
        disconnect();
    }

    if (deviceID.empty() || deviceID == NONE_DEVICE_ID) {
        m_deviceID = deviceID;
        return muse::make_ok();
    }

    if (deviceID == ALL_DEVICES_ID) {
        m_deviceID = deviceID;

        for (const MidiDevice& device : availableDevices()) {
            if (device.id == NONE_DEVICE_ID || device.id == ALL_DEVICES_ID) {
                continue;
            }

            // e.g. a device used by another application can't be opened: the others are still listened to
            Ret ret = openDevice(device.id);
            if (!ret) {
                LOGW() << ret.text();
            }
        }

        LOGI() << "Connected to " << m_deviceID << ", opened devices: " << m_win->inputs.size();
        return muse::make_ok();
    }

    Ret ret = openDevice(deviceID);
    if (ret) {
        m_deviceID = deviceID;
        LOGI() << "Connected to " << m_deviceID;
    }

    return ret;
}

Ret WinMidiInPort::openDevice(const MidiDeviceID& deviceID)
{
    std::vector<int> deviceParams = splitDeviceId(deviceID);
    IF_ASSERT_FAILED(deviceParams.size() == 3) {
        return make_ret(Err::MidiInvalidDeviceID, "invalid device id: " + deviceID);
    }

    auto input = std::make_unique<Win::Input>();
    MMRESULT openRes = midiInOpen(&input->midiIn, deviceParams.at(0),
                                  reinterpret_cast<DWORD_PTR>(&process),
                                  reinterpret_cast<DWORD_PTR>(this),
                                  CALLBACK_FUNCTION | MIDI_IO_STATUS);

    if (openRes != MMSYSERR_NOERROR) {
        return make_ret(Err::MidiFailedConnect, "failed open port " + deviceID + ", error: " + wmidi_prv::errorString(openRes));
    }

    input->buffer.resize(1024, 0);
    input->header.lpData = reinterpret_cast<LPSTR>(input->buffer.data());
    input->header.dwBufferLength = static_cast<DWORD>(input->buffer.size());

    midiInPrepareHeader(input->midiIn, &input->header, sizeof(MIDIHDR));
    midiInAddBuffer(input->midiIn, &input->header, sizeof(MIDIHDR));
    midiInStart(input->midiIn);

    m_win->inputs.push_back(std::move(input));

    return muse::make_ok();
}

void WinMidiInPort::disconnect()
{
    if (!isConnected()) {
        return;
    }

    m_closing = true;
    for (const std::unique_ptr<Win::Input>& input : m_win->inputs) {
        midiInStop(input->midiIn);
        midiInReset(input->midiIn);
        midiInUnprepareHeader(input->midiIn, &input->header, sizeof(MIDIHDR));
        midiInClose(input->midiIn);
    }
    m_win->inputs.clear();
    m_closing = false;

    LOGI() << "Disconnected from " << m_deviceID;

    m_deviceID.clear();
}

bool WinMidiInPort::isConnected() const
{
    return !m_deviceID.empty();
}

MidiDeviceID WinMidiInPort::deviceID() const
{
    return m_deviceID;
}

async::Notification WinMidiInPort::deviceChanged() const
{
    return m_deviceChanged;
}

async::Channel<tick_t, Event> WinMidiInPort::eventReceived() const
{
    return m_eventReceived;
}
