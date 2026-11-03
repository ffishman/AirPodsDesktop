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

#include "BtAudio_win.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <propvarutil.h>

#include "../Logger.h"

namespace Core::BtAudio {

ScopedComApartment::ScopedComApartment()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    _uninitialize = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        LOG(Warn, "CoInitializeEx failed. HRESULT: {:#x}", result);
    }
}

ScopedComApartment::~ScopedComApartment()
{
    if (_uninitialize) {
        CoUninitialize();
    }
}

QString GuidToString(const GUID &guid)
{
    wchar_t buffer[39]{};
    if (StringFromGUID2(guid, buffer, 39) == 0) {
        return {};
    }
    return QString::fromWCharArray(buffer);
}

QString NormalizeContainerId(QString id)
{
    id = id.trimmed();
    if (id.startsWith('{') && id.endsWith('}')) {
        id = id.mid(1, id.size() - 2);
    }
    return id.toLower();
}

QString CanonicalContainerId(const QString &id)
{
    const auto normalized = NormalizeContainerId(id);
    if (normalized.isEmpty()) {
        return {};
    }
    return "{" + normalized.toUpper() + "}";
}

QString MaskDeviceId(const QString &id)
{
    const auto trimmed = id.trimmed();
    if (trimmed.isEmpty()) {
        return "<empty>";
    }
    return "***" + trimmed.right(4);
}

QString ContainerIdFromProperty(const PROPVARIANT &value)
{
    if (value.vt == VT_CLSID && value.puuid != nullptr) {
        return GuidToString(*value.puuid);
    }
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        return QString::fromWCharArray(value.pwszVal);
    }
    if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
        return QString::fromWCharArray(value.bstrVal);
    }
    return {};
}

OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> CreateDeviceEnumerator()
{
    OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> enumerator;
    const HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, enumerator.GetIID(),
        reinterpret_cast<void **>(enumerator.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        LOG(Warn, "Create MMDeviceEnumerator failed. HRESULT: {:#x}", result);
        return {};
    }
    return enumerator;
}

std::optional<QString> GetEndpointContainerId(IMMDevice &endpoint)
{
    OS::Windows::Com::UniquePtr<IPropertyStore> propertyStore;
    HRESULT result = endpoint.OpenPropertyStore(STGM_READ, propertyStore.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "Open endpoint property store failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    PROPVARIANT containerValue;
    PropVariantInit(&containerValue);
    result = propertyStore->GetValue(PKEY_Device_ContainerId, &containerValue);
    if (FAILED(result)) {
        LOG(Warn, "Read endpoint container id failed. HRESULT: {:#x}", result);
        PropVariantClear(&containerValue);
        return std::nullopt;
    }

    const auto containerId = CanonicalContainerId(ContainerIdFromProperty(containerValue));
    PropVariantClear(&containerValue);
    if (containerId.isEmpty()) {
        return std::nullopt;
    }
    return containerId;
}

std::vector<EndpointInfo> EnumerateAudioEndpoints(std::stop_token stopToken)
{
    ScopedComApartment apartment;

    std::vector<EndpointInfo> endpoints;
    if (stopToken.stop_requested()) {
        return endpoints;
    }

    auto enumerator = CreateDeviceEnumerator();
    if (!enumerator) {
        return endpoints;
    }

    OS::Windows::Com::UniquePtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator->EnumAudioEndpoints(
        eAll, DEVICE_STATEMASK_ALL, collection.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "EnumAudioEndpoints failed. HRESULT: {:#x}", result);
        return endpoints;
    }

    UINT count{};
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        LOG(Warn, "IMMDeviceCollection::GetCount failed. HRESULT: {:#x}", result);
        return endpoints;
    }

    endpoints.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        if (stopToken.stop_requested()) {
            break;
        }

        OS::Windows::Com::UniquePtr<IMMDevice> endpoint;
        result = collection->Item(i, endpoint.ReleaseAndAddressOf());
        if (FAILED(result)) {
            LOG(Warn, "IMMDeviceCollection::Item failed. HRESULT: {:#x}", result);
            continue;
        }

        LPWSTR rawEndpointId{};
        result = endpoint->GetId(&rawEndpointId);
        if (FAILED(result)) {
            LOG(Warn, "IMMDevice::GetId failed. HRESULT: {:#x}", result);
            continue;
        }
        const QString endpointId = QString::fromWCharArray(rawEndpointId);
        CoTaskMemFree(rawEndpointId);

        auto containerId = GetEndpointContainerId(*endpoint.operator->());
        if (!containerId.has_value()) {
            continue;
        }

        DWORD state{};
        result = endpoint->GetState(&state);
        if (FAILED(result)) {
            LOG(Warn, "IMMDevice::GetState failed. HRESULT: {:#x}", result);
            continue;
        }

        endpoints.push_back({endpointId, containerId.value(), state});
    }

    return endpoints;
}

std::optional<QString> GetConnectedFilterDeviceId(IMMDevice &endpoint)
{
    OS::Windows::Com::UniquePtr<IDeviceTopology> topology;
    HRESULT result = endpoint.Activate(
        topology.GetIID(), CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(topology.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        LOG(Warn, "Activate IDeviceTopology failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IConnector> connector;
    result = topology->GetConnector(0, connector.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "IDeviceTopology::GetConnector failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IConnector> connectedConnector;
    result = connector->GetConnectedTo(connectedConnector.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "IConnector::GetConnectedTo failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IPart> part;
    result = connectedConnector->QueryInterface(
        part.GetIID(), reinterpret_cast<void **>(part.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        LOG(Warn, "Query IPart failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IDeviceTopology> connectedTopology;
    result = part->GetTopologyObject(connectedTopology.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "Get connected topology failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    LPWSTR rawConnectedDeviceId{};
    result = connectedTopology->GetDeviceId(&rawConnectedDeviceId);
    if (FAILED(result)) {
        LOG(Warn, "Get connected device id failed. HRESULT: {:#x}", result);
        return std::nullopt;
    }

    QString filterDeviceId = QString::fromWCharArray(rawConnectedDeviceId);
    CoTaskMemFree(rawConnectedDeviceId);
    return filterDeviceId;
}

bool IsBluetoothFilterDeviceId(const QString &filterDeviceId)
{
    // A2DP sinks are enumerated by BTHENUM, hands-free endpoints by BTHHFENUM.
    return filterDeviceId.contains("bthenum", Qt::CaseInsensitive) ||
           filterDeviceId.contains("bthhfenum", Qt::CaseInsensitive);
}

HRESULT SendOneShot(const QString &endpointId, OneShot oneShot, std::stop_token stopToken)
{
    ScopedComApartment apartment;

    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    auto enumerator = CreateDeviceEnumerator();
    if (!enumerator) {
        return E_FAIL;
    }

    OS::Windows::Com::UniquePtr<IMMDevice> endpoint;
    const auto endpointIdW = endpointId.toStdWString();
    HRESULT result = enumerator->GetDevice(endpointIdW.c_str(), endpoint.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "Get endpoint for BtAudio request failed. HRESULT: {:#x}", result);
        return result;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    const auto filterDeviceId = GetConnectedFilterDeviceId(*endpoint.operator->());
    if (!filterDeviceId.has_value()) {
        LOG(Warn, "Resolve KS filter failed. Endpoint: {}", MaskDeviceId(endpointId));
        return E_FAIL;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    OS::Windows::Com::UniquePtr<IMMDevice> connectedDevice;
    const auto filterDeviceIdW = filterDeviceId->toStdWString();
    result = enumerator->GetDevice(filterDeviceIdW.c_str(), connectedDevice.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "Get connected device failed. Endpoint: {}, HRESULT: {:#x}",
            MaskDeviceId(endpointId), result);
        return result;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    OS::Windows::Com::UniquePtr<IKsControl> ksControl;
    result = connectedDevice->Activate(
        ksControl.GetIID(), CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(ksControl.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        LOG(Warn, "Activate IKsControl failed. Endpoint: {}, HRESULT: {:#x}",
            MaskDeviceId(endpointId), result);
        return result;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    KSPROPERTY property{
        KSPROPSETID_BtAudio,
        oneShot == OneShot::Reconnect ? static_cast<ULONG>(KSPROPERTY_ONESHOT_RECONNECT)
                                      : static_cast<ULONG>(KSPROPERTY_ONESHOT_DISCONNECT),
        KSPROPERTY_TYPE_GET,
    };
    ULONG returned{};
    result = ksControl->KsProperty(
        reinterpret_cast<PKSPROPERTY>(&property), sizeof(property), nullptr, 0, &returned);
    if (FAILED(result)) {
        LOG(Warn, "BtAudio {} request failed. Endpoint: {}, HRESULT: {:#x}",
            oneShot == OneShot::Reconnect ? "reconnect" : "disconnect", MaskDeviceId(endpointId),
            result);
    }
    return result;
}

} // namespace Core::BtAudio
