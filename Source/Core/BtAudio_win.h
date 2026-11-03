//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#if !defined APD_OS_WIN
    #error "This file shouldn't be compiled."
#endif

// Shared MMDevice / kernel-streaming helpers for Bluetooth audio endpoints.
//
// Quick connect and audio stall recovery both need to map an MMDevice endpoint to its Bluetooth
// container and to send the BtAudio one-shot properties (`KSPROPERTY_ONESHOT_RECONNECT` and
// `KSPROPERTY_ONESHOT_DISCONNECT`) to the driver behind it. Those are the same requests the
// Windows Settings app issues for "Connect" and "Disconnect", and they need no elevation.

#include "OS/Windows.h"

#include <optional>
#include <stop_token>
#include <vector>

#include <QString>

#include <mmdeviceapi.h>

namespace Core::BtAudio {

class ScopedComApartment final
{
public:
    ScopedComApartment();
    ~ScopedComApartment();

    ScopedComApartment(const ScopedComApartment &) = delete;
    ScopedComApartment &operator=(const ScopedComApartment &) = delete;

private:
    bool _uninitialize{};
};

struct EndpointInfo {
    QString id;
    QString containerId;
    DWORD state{};
};

enum class OneShot : uint32_t { Reconnect, Disconnect };

QString GuidToString(const GUID &guid);
QString NormalizeContainerId(QString id);
QString CanonicalContainerId(const QString &id);

// Container ids and MMDevice endpoint ids are stable identifiers for the user's hardware, the same
// class of data the settings layer redacts for `device_address`. Log just enough of one to
// correlate lines within a single session.
QString MaskDeviceId(const QString &id);

QString ContainerIdFromProperty(const PROPVARIANT &value);

OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> CreateDeviceEnumerator();
std::optional<QString> GetEndpointContainerId(IMMDevice &endpoint);

// Every render and capture endpoint in any state, each with its container id. Endpoints without a
// container id are skipped. Initializes COM on the calling thread for the duration of the call.
std::vector<EndpointInfo> EnumerateAudioEndpoints(std::stop_token stopToken);

// Device id of the kernel-streaming filter the endpoint is connected to, for example
// `{2}.\\?\bthenum#...` for an A2DP sink. Requires COM on the calling thread.
std::optional<QString> GetConnectedFilterDeviceId(IMMDevice &endpoint);

// True when the filter id belongs to the Microsoft Bluetooth audio stack (A2DP or hands-free).
bool IsBluetoothFilterDeviceId(const QString &filterDeviceId);

// Sends a BtAudio one-shot property to the driver behind `endpointId`. Returns the HRESULT of the
// request; S_OK means the driver accepted it, not that the link state already changed.
// Initializes COM on the calling thread for the duration of the call.
HRESULT SendOneShot(const QString &endpointId, OneShot oneShot, std::stop_token stopToken);

} // namespace Core::BtAudio
