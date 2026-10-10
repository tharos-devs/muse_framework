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
#include "coremidiinport.h"

#include <CoreAudio/HostTime.h>
#include <CoreServices/CoreServices.h>
#include <CoreMIDI/CoreMIDI.h>

#include "containers.h"
#include "translation.h"
#include "midierrors.h"
#include "defer.h"
#include "log.h"

using namespace muse;
using namespace muse::midi;

static_assert(std::is_same_v<MIDIEndpointRef, uint32_t>, "CoreMidiInPort::connectSource() takes a MIDIEndpointRef as uint32_t");

//#define DEBUG_COREMIDIINPORT
#ifdef DEBUG_COREMIDIINPORT
#define LOG_MIDI_D LOGD
#define LOG_MIDI_W LOGW
#else
#define LOG_MIDI_D LOGN
#define LOG_MIDI_W LOGN
#endif

struct muse::midi::CoreMidiInPort::Core {
    MIDIClientRef client = 0;
    MIDIPortRef inputPort = 0;
    std::vector<MIDIEndpointRef> sources; // the connected ones: one, or every source with ALL_DEVICES_ID
};

CoreMidiInPort::CoreMidiInPort()
    : m_core(std::make_unique<Core>())
{
}

CoreMidiInPort::~CoreMidiInPort()
{
}

void CoreMidiInPort::init()
{
    initCore();
}

void CoreMidiInPort::deinit()
{
    if (isConnected()) {
        disconnect();
    }

    if (m_core->inputPort) {
        MIDIPortDispose(m_core->inputPort);
    }

    if (m_core->client) {
        MIDIClientDispose(m_core->client);
    }
}

MidiDeviceList CoreMidiInPort::availableDevices() const
{
    MidiDeviceList ret;

    ret.push_back({ NONE_DEVICE_ID, muse::trc("midi", "No device") });
    ret.push_back({ ALL_DEVICES_ID, muse::trc("midi", "All devices") });

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    ItemCount sources = MIDIGetNumberOfSources();
    for (ItemCount sourceIndex = 0; sourceIndex <= sources; sourceIndex++) {
        MIDIEndpointRef sourceRef = MIDIGetSource(sourceIndex);
        if (sourceRef != 0) {
            MidiDevice dev;

            SInt32 id = 0;
            if (MIDIObjectGetIntegerProperty(sourceRef, kMIDIPropertyUniqueID, &id) != noErr) {
                LOGE() << "Can't get property kMIDIPropertyUniqueID";
                continue;
            }

            CFStringRef stringRef = 0;
            char name[256];
            if (MIDIObjectGetStringProperty(sourceRef, kMIDIPropertyDisplayName, &stringRef) != noErr) {
                LOGE() << "Can't get property kMIDIPropertyDisplayName";
                continue;
            }
            CFStringGetCString(stringRef, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(stringRef);

            dev.id = std::to_string(id);
            dev.name = name;

            ret.push_back(std::move(dev));
        }
    }

    return ret;
}

async::Notification CoreMidiInPort::availableDevicesChanged() const
{
    return m_availableDevicesChanged;
}

void CoreMidiInPort::initCore()
{
    OSStatus result;

    static auto onCoreMidiNotificationReceived = [](const MIDINotification* notification, void* refCon) {
        auto self = static_cast<CoreMidiInPort*>(refCon);
        IF_ASSERT_FAILED(self) {
            return;
        }

        switch (notification->messageID) {
        case kMIDIMsgObjectAdded:
        case kMIDIMsgObjectRemoved: {
            if (notification->messageSize != sizeof(MIDIObjectAddRemoveNotification)) {
                LOGW() << "Received corrupted MIDIObjectAddRemoveNotification";
                break;
            }

            auto addRemoveNotification = (const MIDIObjectAddRemoveNotification*)notification;

            if (addRemoveNotification->childType != kMIDIObjectType_Source) {
                break;
            }

            const MIDIEndpointRef source = (MIDIEndpointRef)addRemoveNotification->child;
            const bool listensToAll = self->m_deviceID == ALL_DEVICES_ID;

            if (notification->messageID == kMIDIMsgObjectAdded) {
                if (listensToAll) {
                    self->connectSource(source);
                }
            } else {
                std::vector<MIDIEndpointRef>& sources = self->m_core->sources;
                if (muse::contains(sources, source)) {
                    if (listensToAll) {
                        // CoreMIDI has already dropped its connection
                        muse::remove(sources, source);
                    } else {
                        self->disconnect();
                    }
                }
            }

            self->availableDevicesChanged().notify();
        } break;

        case kMIDIMsgPropertyChanged: {
            if (notification->messageSize != sizeof(MIDIObjectPropertyChangeNotification)) {
                LOGW() << "Received corrupted MIDIObjectPropertyChangeNotification";
                break;
            }

            auto propertyChangeNotification = (const MIDIObjectPropertyChangeNotification*)notification;

            if (propertyChangeNotification->objectType != kMIDIObjectType_Device
                && propertyChangeNotification->objectType != kMIDIObjectType_Source) {
                break;
            }

            if (CFStringCompare(propertyChangeNotification->propertyName, kMIDIPropertyDisplayName, 0) == kCFCompareEqualTo
                || CFStringCompare(propertyChangeNotification->propertyName, kMIDIPropertyName, 0) == kCFCompareEqualTo) {
                self->availableDevicesChanged().notify();
            }
        } break;

        // General message that should be ignored because we handle specific ones
        case kMIDIMsgSetupChanged:

        case kMIDIMsgThruConnectionsChanged:
        case kMIDIMsgSerialPortOwnerChanged:

        case kMIDIMsgIOError:
            break;
        }
    };

    result = MIDIClientCreate(CFSTR("MuseScore"), onCoreMidiNotificationReceived, this, &m_core->client);
    IF_ASSERT_FAILED(result == noErr) {
        LOGE() << "failed create midi input client";
        return;
    }

    CFStringRef portName = CFSTR("MuseScore MIDI input port");
    if (__builtin_available(macOS 11.0, *)) {
        MIDIReceiveBlock receiveBlock = ^ (const MIDIEventList* eventList, void* /*srcConnRefCon*/) {
            const MIDIEventPacket* packet = eventList->packet;
            for (UInt32 index = 0; index < eventList->numPackets; index++) {
                LOG_MIDI_D() << "Receiving MIDIEventPacket with " << packet->wordCount << " words";
                // Handle packet
                uint32_t pos = 0;
                while (pos < packet->wordCount) {
                    uint32_t messageType = packet->words[pos] >> 28;
                    size_t messageWordCount = Event::wordCountForMessageType(static_cast<Event::MessageType>(messageType));

                    LOG_MIDI_D() << "Receiving midi message with " << messageWordCount << " words";
                    Event e = Event::fromMidi20Words(&packet->words[pos], messageWordCount);
                    if (e) {
                        LOG_MIDI_D() << "Received midi message: " << e.to_string();
                        m_eventReceived.send((tick_t)packet->timeStamp, e);
                    }
                    pos += messageWordCount;
                }
                packet = MIDIEventPacketNext(packet);
            }
        };

        result
            = MIDIInputPortCreateWithProtocol(m_core->client, portName, kMIDIProtocol_2_0, &m_core->inputPort, receiveBlock);
    } else {
        MIDIReadBlock readBlock = ^ (const MIDIPacketList* packetList, void* /*srcConnRefCon*/)
        {
            const MIDIPacket* packet = packetList->packet;
            for (UInt32 index = 0; index < packetList->numPackets; index++) {
                auto len = packet->length;
                int pos = 0;
                const Byte* pointer = static_cast<const Byte*>(&(packet->data[0]));
                while (pos < len) {
                    Byte status = pointer[pos] >> 4;
                    if (status < 8 || status >= 15) {
                        LOG_MIDI_W() << "Unhandled status byte: " << status;
                        return;
                    }
                    Event::Opcode opcode = static_cast<Event::Opcode>(status);
                    size_t msgLen = Event::midi10ByteCountForOpcode(opcode);
                    if (msgLen == 0) {
                        LOG_MIDI_W() << "Unhandled opcode:" << status;
                        return;
                    }
                    Event e = Event::fromMidi10Bytes(pointer + pos, msgLen);
                    LOG_MIDI_D() << "Received midi 1.0 message: " << e.to_string();
                    e = e.toMIDI20();
                    if (e) {
                        LOG_MIDI_D() << "Converted to midi 2.0 midi message: " << e.to_string();
                        m_eventReceived.send((tick_t)packet->timeStamp, e);
                    }
                    pos += msgLen;
                }
                packet = MIDIPacketNext(packet);
            }
        };

        result = MIDIInputPortCreateWithBlock(m_core->client, portName, &m_core->inputPort, readBlock);
    }

    IF_ASSERT_FAILED(result == noErr) {
        LOGE() << "failed create midi input port";
    }
}

Ret CoreMidiInPort::connect(const MidiDeviceID& deviceID)
{
    DEFER {
        m_deviceChanged.notify();
    };

    if (isConnected()) {
        disconnect();
    }

    Ret ret = muse::make_ok();

    if (!deviceID.empty() && deviceID != NONE_DEVICE_ID) {
        if (!m_core->client) {
            return make_ret(Err::MidiFailedConnect, "failed create client");
        }

        if (!m_core->inputPort) {
            return make_ret(Err::MidiFailedConnect, "failed create port");
        }

        if (deviceID == ALL_DEVICES_ID) {
            m_deviceID = deviceID;

            const ItemCount count = MIDIGetNumberOfSources();
            for (ItemCount i = 0; i < count; ++i) {
                if (MIDIEndpointRef source = MIDIGetSource(i)) {
                    connectSource(source);
                }
            }
        } else {
            MIDIObjectRef obj;
            MIDIObjectType type;

            OSStatus err = MIDIObjectFindByUniqueID(std::stoi(deviceID), &obj, &type);
            if (err != noErr) {
                return make_ret(Err::MidiFailedConnect, "failed get source");
            }

            ret = connectSource((MIDIEndpointRef)obj);
            if (ret) {
                m_deviceID = deviceID;
            }
        }
    } else {
        m_deviceID = deviceID;
    }

    if (ret) {
        LOGD() << "Connected to " << m_deviceID;
    }

    return ret;
}

void CoreMidiInPort::disconnect()
{
    if (!isConnected()) {
        return;
    }

    for (MIDIEndpointRef source : m_core->sources) {
        OSStatus result = MIDIPortDisconnectSource(m_core->inputPort, source);
        if (result != noErr && result != kMIDINoConnection) {
            LOGE() << "can't disconnect midi port " << result;
        }
    }
    m_core->sources.clear();

    LOGD() << "Disconnected from " << m_deviceID;

    m_deviceID.clear();
}

//! NOTE: listening to all devices stays connected without any device plugged in, so that the next one is listened to
bool CoreMidiInPort::isConnected() const
{
    return !m_deviceID.empty() && (m_deviceID == ALL_DEVICES_ID || !m_core->sources.empty());
}

MidiDeviceID CoreMidiInPort::deviceID() const
{
    return m_deviceID;
}

async::Notification CoreMidiInPort::deviceChanged() const
{
    return m_deviceChanged;
}

async::Channel<tick_t, Event> CoreMidiInPort::eventReceived() const
{
    return m_eventReceived;
}

Ret CoreMidiInPort::connectSource(uint32_t source)
{
    if (muse::contains(m_core->sources, source)) {
        return muse::make_ok();
    }

    OSStatus result = MIDIPortConnectSource(m_core->inputPort, source, nullptr /*connRefCon*/);
    if (result != noErr) {
        LOGW() << "failed connect source, err: " << result;
        return make_ret(Err::MidiFailedConnect);
    }

    m_core->sources.push_back(source);
    return muse::make_ok();
}
