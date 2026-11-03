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

#include "QuickConnect_win.h"

#include "BtAudio_win.h"

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <map>
#include <set>

#include <QMetaObject>

#include "../Logger.h"

namespace Core::QuickConnect {
namespace {

using BtAudio::CanonicalContainerId;
using BtAudio::EnumerateAudioEndpoints;
using BtAudio::GuidToString;
using BtAudio::MaskDeviceId;
using BtAudio::NormalizeContainerId;

constexpr auto kPropertyAepContainerId = L"System.Devices.Aep.ContainerId";

GUID ToGuid(const winrt::guid &guid)
{
    return {
        guid.Data1,
        guid.Data2,
        guid.Data3,
        {
            guid.Data4[0],
            guid.Data4[1],
            guid.Data4[2],
            guid.Data4[3],
            guid.Data4[4],
            guid.Data4[5],
            guid.Data4[6],
            guid.Data4[7],
        },
    };
}

QString ContainerIdFromInspectable(const winrt::Windows::Foundation::IInspectable &inspectable)
{
    if (!inspectable) {
        return {};
    }

    try {
        return QString::fromWCharArray(winrt::unbox_value<winrt::hstring>(inspectable).c_str());
    }
    catch (const winrt::hresult_error &) {
    }

    try {
        return GuidToString(ToGuid(winrt::unbox_value<winrt::guid>(inspectable)));
    }
    catch (const winrt::hresult_error &) {
    }

    return {};
}

std::map<QString, Device> EnumeratePairedBluetoothDevices(std::stop_token stopToken)
{
    OS::Windows::Winrt::Initialize();

    namespace WinrtBluetooth = winrt::Windows::Devices::Bluetooth;
    namespace WinrtDevices = winrt::Windows::Devices::Enumeration;

    std::map<QString, Device> devices;

    try {
        auto properties = winrt::single_threaded_vector<winrt::hstring>();
        properties.Append(kPropertyAepContainerId);

        const auto operation = WinrtDevices::DeviceInformation::FindAllAsync(
            WinrtBluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true), properties);
        std::stop_callback cancelOperation{stopToken, [operation]() mutable noexcept {
                                               try {
                                                   operation.Cancel();
                                               }
                                               catch (...) {
                                               }
                                           }};
        const auto collection = operation.get();

        for (uint32_t i = 0; i < collection.Size(); ++i) {
            if (stopToken.stop_requested()) {
                break;
            }

            const auto info = collection.GetAt(i);
            const auto rawContainerId =
                ContainerIdFromInspectable(info.Properties().TryLookup(kPropertyAepContainerId));
            const auto containerId = CanonicalContainerId(rawContainerId);
            const auto containerKey = NormalizeContainerId(containerId);
            if (containerKey.isEmpty()) {
                continue;
            }

            auto name = QString::fromWCharArray(info.Name().c_str());
            if (name.isEmpty()) {
                name = containerId;
            }

            devices.try_emplace(containerKey, Device{containerId, name, false});
        }
    }
    catch (const OS::Windows::Winrt::Exception &ex) {
        if (!stopToken.stop_requested()) {
            LOG(Warn, "Bluetooth paired device enumeration failed. {}", Helper::ToString(ex));
        }
    }

    return devices;
}

} // namespace

WindowsBackend::WindowsBackend()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    _comInitialized = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        LOG(Warn, "WindowsBackend COM initialization failed. HRESULT: {:#x}", result);
    }

    RegisterNotifications();
}

WindowsBackend::~WindowsBackend()
{
    // Stop new callbacks first, then drop the snapshot, so a callback that is already past the
    // unregister point finds nothing to observe and returns without touching the Controller.
    UnregisterNotifications();
    StopObserving();
    SetController(nullptr);

    if (_comInitialized) {
        CoUninitialize();
    }
}

void WindowsBackend::SetController(Controller *controller)
{
    std::lock_guard<std::mutex> lock{_controllerMutex};
    _controller = controller;
}

void WindowsBackend::StopObserving()
{
    _observation.store({});
}

std::vector<Device> WindowsBackend::ListDevices(std::stop_token stopToken)
{
    auto pairedDevices = EnumeratePairedBluetoothDevices(stopToken);
    if (stopToken.stop_requested()) {
        return {};
    }
    const auto endpoints = EnumerateAudioEndpoints(stopToken);

    std::set<QString> endpointContainerKeys;
    std::set<QString> activeEndpointContainerKeys;
    for (const auto &endpoint : endpoints) {
        const auto key = NormalizeContainerId(endpoint.containerId);
        if (key.isEmpty()) {
            continue;
        }
        endpointContainerKeys.insert(key);
        if (endpoint.state == DEVICE_STATE_ACTIVE) {
            activeEndpointContainerKeys.insert(key);
        }
    }

    std::vector<Device> devices;
    devices.reserve(pairedDevices.size());
    for (auto &[containerKey, device] : pairedDevices) {
        if (!endpointContainerKeys.contains(containerKey)) {
            continue;
        }

        device.connected = activeEndpointContainerKeys.contains(containerKey);
        devices.push_back(device);
    }

    return devices;
}

bool WindowsBackend::RequestReconnect(const QString &id, std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return false;
    }

    const auto selectedContainerKey = NormalizeContainerId(id);
    if (selectedContainerKey.isEmpty()) {
        return false;
    }

    // Resolve the endpoints belonging to this container up front, so the notification callback can
    // recognise them with a plain string compare instead of re-entering the MMDevice API.
    auto observation = std::make_shared<Observation>();
    observation->containerId = CanonicalContainerId(id);

    const auto endpoints = EnumerateAudioEndpoints(stopToken);
    for (const auto &endpoint : endpoints) {
        if (NormalizeContainerId(endpoint.containerId) != selectedContainerKey) {
            continue;
        }
        observation->endpointIds.insert(endpoint.id);
    }
    _observation.store(std::const_pointer_cast<const Observation>(observation));

    bool accepted = false;
    for (const auto &endpointId : observation->endpointIds) {
        if (stopToken.stop_requested()) {
            break;
        }

        const HRESULT result =
            BtAudio::SendOneShot(endpointId, BtAudio::OneShot::Reconnect, stopToken);
        accepted = accepted || result == S_OK;
    }

    if (!accepted) {
        StopObserving();
        if (!stopToken.stop_requested()) {
            LOG(Warn, "No endpoint accepted BtAudio reconnect request. Container: {}",
                MaskDeviceId(id));
        }
    }
    return accepted;
}

ULONG WindowsBackend::AddRef()
{
    return ++_refCount;
}

ULONG WindowsBackend::Release()
{
    const auto count = --_refCount;
    return count;
}

HRESULT WindowsBackend::QueryInterface(REFIID riid, void **object)
{
    if (object == nullptr) {
        return E_POINTER;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient *>(this);
        AddRef();
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

// Runs on an MMDevice notification thread. It must stay nonblocking and must not call back into
// the MMDevice API, so everything it needs was resolved by `RequestReconnect` beforehand and is
// read here as a single lock-free snapshot.
HRESULT WindowsBackend::OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState)
{
    if (deviceId == nullptr) {
        return S_OK;
    }

    const auto observation = _observation.load();
    if (!observation || !observation->endpointIds.contains(QString::fromWCharArray(deviceId))) {
        return S_OK;
    }

    QueueEndpointState(observation->containerId, newState == DEVICE_STATE_ACTIVE);
    return S_OK;
}

HRESULT WindowsBackend::OnDeviceAdded(LPCWSTR deviceId)
{
    Q_UNUSED(deviceId)
    return S_OK;
}

HRESULT WindowsBackend::OnDeviceRemoved(LPCWSTR deviceId)
{
    Q_UNUSED(deviceId)
    return S_OK;
}

HRESULT WindowsBackend::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId)
{
    Q_UNUSED(flow)
    Q_UNUSED(role)
    Q_UNUSED(defaultDeviceId)
    return S_OK;
}

HRESULT WindowsBackend::OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key)
{
    Q_UNUSED(deviceId)
    Q_UNUSED(key)
    return S_OK;
}

void WindowsBackend::RegisterNotifications()
{
    _deviceEnumerator = BtAudio::CreateDeviceEnumerator();
    if (!_deviceEnumerator) {
        return;
    }

    const HRESULT result = _deviceEnumerator->RegisterEndpointNotificationCallback(this);
    if (FAILED(result)) {
        LOG(Warn, "RegisterEndpointNotificationCallback failed. HRESULT: {:#x}", result);
        _deviceEnumerator = nullptr;
    }
}

void WindowsBackend::UnregisterNotifications()
{
    if (!_deviceEnumerator) {
        return;
    }

    const HRESULT result = _deviceEnumerator->UnregisterEndpointNotificationCallback(this);
    if (FAILED(result)) {
        LOG(Warn, "UnregisterEndpointNotificationCallback failed. HRESULT: {:#x}", result);
    }
    _deviceEnumerator = nullptr;
}

void WindowsBackend::QueueEndpointState(const QString &containerId, bool connected)
{
    // `_controllerMutex` is only ever held for a pointer copy and the queued post, neither of which
    // can block, so taking it here cannot stall the notification thread. It is what keeps
    // `~Controller` from detaching between the null check and the post; the Controller clears
    // itself before it starts tearing down, and Qt drops queued metacalls for a destroyed object.
    std::lock_guard<std::mutex> lock{_controllerMutex};
    if (_controller == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(
        _controller,
        [controller = _controller, containerId, connected]() {
            controller->OnEndpointStateChanged(containerId, connected);
        },
        Qt::QueuedConnection);
}

} // namespace Core::QuickConnect
