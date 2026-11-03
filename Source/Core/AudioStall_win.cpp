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

#include "AudioStall_win.h"

#include <thread>
#include <vector>

#include "BtAudio_win.h"
#include "../Logger.h"

namespace Core::AudioStall {

void WindowsBackend::ThreadAttach()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    _comInitialized = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        LOG(Warn, "AudioStall: CoInitializeEx failed. HRESULT: {:#x}", result);
    }
}

void WindowsBackend::ThreadDetach()
{
    StopProbe();
    ReleaseEndpoint();
    _enumerator = nullptr;

    if (_comInitialized) {
        CoUninitialize();
        _comInitialized = false;
    }
}

Sample WindowsBackend::Poll()
{
    Sample sample;

    if (!_enumerator) {
        _enumerator = BtAudio::CreateDeviceEnumerator();
        if (!_enumerator) {
            return sample;
        }
    }

    if (!ResolveDefaultEndpoint() || !_bluetooth) {
        StopProbe();
        return sample;
    }

    sample.eligible = true;
    sample.sessionsActive = AnyForeignSessionActive();
    sample.endpointPeak = ReadPeak();

    // A failure above may have invalidated the endpoint; judge again on the next poll.
    if (!_device) {
        return Sample{};
    }

    if (!sample.sessionsActive) {
        StopProbe();
        return sample;
    }

    if (!_probeClient && !StartProbe()) {
        return sample;
    }
    sample.probeAdvancing = TickProbe();
    return sample;
}

bool WindowsBackend::Recover(std::stop_token stopToken)
{
    const auto containerKey = BtAudio::NormalizeContainerId(_containerId);

    // Close our own handles first so nothing of ours holds the stream across the toggle.
    StopProbe();
    ReleaseEndpoint();

    if (containerKey.isEmpty()) {
        LOG(Warn, "AudioStall: the stalled endpoint has no container id, cannot recover.");
        return false;
    }

    std::vector<QString> activeIds, allIds;
    for (const auto &endpoint : BtAudio::EnumerateAudioEndpoints(stopToken)) {
        if (BtAudio::NormalizeContainerId(endpoint.containerId) != containerKey) {
            continue;
        }
        allIds.push_back(endpoint.id);
        if (endpoint.state == DEVICE_STATE_ACTIVE) {
            activeIds.push_back(endpoint.id);
        }
    }

    LOG(Info, "AudioStall: recovering container {}. active endpoints: {}, all endpoints: {}",
        BtAudio::MaskDeviceId(containerKey), activeIds.size(), allIds.size());

    if (allIds.empty()) {
        return false;
    }

    size_t disconnectAccepted = 0;
    for (const auto &id : activeIds) {
        if (stopToken.stop_requested()) {
            return false;
        }
        if (BtAudio::SendOneShot(id, BtAudio::OneShot::Disconnect, stopToken) == S_OK) {
            ++disconnectAccepted;
        }
    }
    LOG(Info, "AudioStall: disconnect accepted by {} of {} endpoints.", disconnectAccepted,
        activeIds.size());

    const auto deadline = std::chrono::steady_clock::now() + kReconnectDelay;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stopToken.stop_requested()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    size_t reconnectAccepted = 0;
    for (const auto &id : allIds) {
        if (stopToken.stop_requested()) {
            break;
        }
        if (BtAudio::SendOneShot(id, BtAudio::OneShot::Reconnect, stopToken) == S_OK) {
            ++reconnectAccepted;
        }
    }
    LOG(Info, "AudioStall: reconnect accepted by {} of {} endpoints.", reconnectAccepted,
        allIds.size());

    return reconnectAccepted > 0;
}

bool WindowsBackend::ResolveDefaultEndpoint()
{
    OS::Windows::Com::UniquePtr<IMMDevice> device;
    HRESULT result =
        _enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.ReleaseAndAddressOf());
    if (FAILED(result)) {
        // E_NOTFOUND simply means no output device at all.
        if (!_endpointId.isEmpty()) {
            LOG(Info, "AudioStall: no default output. HRESULT: {:#x}", result);
        }
        StopProbe();
        ReleaseEndpoint();
        return false;
    }

    LPWSTR rawId{};
    result = device->GetId(&rawId);
    if (FAILED(result)) {
        LOG(Warn, "AudioStall: IMMDevice::GetId failed. HRESULT: {:#x}", result);
        return false;
    }
    const QString endpointId = QString::fromWCharArray(rawId);
    CoTaskMemFree(rawId);

    if (_device && endpointId == _endpointId) {
        return true;
    }

    StopProbe();
    ReleaseEndpoint();

    _device = std::move(device);
    _endpointId = endpointId;

    const auto filterDeviceId = BtAudio::GetConnectedFilterDeviceId(*_device.operator->());
    _bluetooth = filterDeviceId.has_value() && BtAudio::IsBluetoothFilterDeviceId(*filterDeviceId);
    _containerId = BtAudio::GetEndpointContainerId(*_device.operator->()).value_or(QString{});

    if (_bluetooth) {
        result = _device->Activate(
            _sessionManager.GetIID(), CLSCTX_ALL, nullptr,
            reinterpret_cast<void **>(_sessionManager.ReleaseAndAddressOf()));
        if (FAILED(result)) {
            LOG(Warn, "AudioStall: activate IAudioSessionManager2 failed. HRESULT: {:#x}", result);
            _bluetooth = false;
        }

        result = _device->Activate(
            _meter.GetIID(), CLSCTX_ALL, nullptr,
            reinterpret_cast<void **>(_meter.ReleaseAndAddressOf()));
        if (FAILED(result)) {
            LOG(Warn, "AudioStall: activate IAudioMeterInformation failed. HRESULT: {:#x}", result);
            _meter = nullptr;
        }
    }

    LOG(Info, "AudioStall: default output is now {} (container {}). bluetooth={}",
        BtAudio::MaskDeviceId(_endpointId), BtAudio::MaskDeviceId(_containerId), _bluetooth);
    return true;
}

void WindowsBackend::ReleaseEndpoint()
{
    _meter = nullptr;
    _sessionManager = nullptr;
    _device = nullptr;
    _endpointId.clear();
    _containerId.clear();
    _bluetooth = false;
}

void WindowsBackend::OnAudioFailure(HRESULT result)
{
    if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        LOG(Info, "AudioStall: endpoint invalidated, it will be resolved again.");
        StopProbe();
        ReleaseEndpoint();
    }
}

bool WindowsBackend::AnyForeignSessionActive()
{
    if (!_sessionManager) {
        return false;
    }

    OS::Windows::Com::UniquePtr<IAudioSessionEnumerator> sessions;
    HRESULT result = _sessionManager->GetSessionEnumerator(sessions.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Trace, "AudioStall: GetSessionEnumerator failed. HRESULT: {:#x}", result);
        OnAudioFailure(result);
        return false;
    }

    int count{};
    result = sessions->GetCount(&count);
    if (FAILED(result)) {
        return false;
    }

    const DWORD self = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
        OS::Windows::Com::UniquePtr<IAudioSessionControl> session;
        if (FAILED(sessions->GetSession(i, session.ReleaseAndAddressOf()))) {
            continue;
        }

        AudioSessionState state{};
        if (FAILED(session->GetState(&state)) || state != AudioSessionStateActive) {
            continue;
        }

        // Our own sessions (the probe, and the low audio latency player) do not count: they
        // would keep the probe alive on their own.
        OS::Windows::Com::UniquePtr<IAudioSessionControl2> sessionEx;
        if (SUCCEEDED(session->QueryInterface(
                sessionEx.GetIID(), reinterpret_cast<void **>(sessionEx.ReleaseAndAddressOf()))))
        {
            DWORD processId{};
            // AUDCLNT_S_NO_SINGLE_PROCESS is a success code and leaves a usable id.
            if (SUCCEEDED(sessionEx->GetProcessId(&processId)) && processId == self) {
                continue;
            }
        }

        return true;
    }

    return false;
}

float WindowsBackend::ReadPeak()
{
    if (!_meter) {
        return 0.f;
    }

    float peak{};
    const HRESULT result = _meter->GetPeakValue(&peak);
    if (FAILED(result)) {
        OnAudioFailure(result);
        return 0.f;
    }
    return peak;
}

bool WindowsBackend::StartProbe()
{
    if (!_device) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (_probeRetryAfter.has_value() && now < *_probeRetryAfter) {
        return false;
    }

    const auto fail = [this, now](const char *what, HRESULT result) {
        LOG(Warn, "AudioStall: probe {} failed. HRESULT: {:#x}. Retrying in {}s.", what, result,
            kProbeRetryDelay.count());
        StopProbe();
        _probeRetryAfter = now + kProbeRetryDelay;
        OnAudioFailure(result);
        return false;
    };

    HRESULT result = _device->Activate(
        _probeClient.GetIID(), CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(_probeClient.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        return fail("activate", result);
    }

    WAVEFORMATEX *format{};
    result = _probeClient->GetMixFormat(&format);
    if (FAILED(result)) {
        return fail("GetMixFormat", result);
    }

    result = _probeClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, kProbeBufferDuration, 0, format,
        nullptr);
    CoTaskMemFree(format);
    if (FAILED(result)) {
        return fail("Initialize", result);
    }

    result = _probeClient->GetBufferSize(&_probeBufferFrames);
    if (FAILED(result)) {
        return fail("GetBufferSize", result);
    }

    result = _probeClient->GetService(
        _probeRender.GetIID(), reinterpret_cast<void **>(_probeRender.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        return fail("GetService(IAudioRenderClient)", result);
    }

    result = _probeClient->GetService(
        _probeClock.GetIID(), reinterpret_cast<void **>(_probeClock.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        return fail("GetService(IAudioClock)", result);
    }

    if (!FillProbe()) {
        return fail("initial fill", E_FAIL);
    }

    result = _probeClient->Start();
    if (FAILED(result)) {
        return fail("Start", result);
    }

    _probeLastPosition.reset();
    _probeRetryAfter.reset();
    LOG(Info, "AudioStall: probe started. buffer frames: {}", _probeBufferFrames);
    return true;
}

void WindowsBackend::StopProbe()
{
    if (_probeClient) {
        _probeClient->Stop();
        LOG(Trace, "AudioStall: probe stopped.");
    }
    _probeClock = nullptr;
    _probeRender = nullptr;
    _probeClient = nullptr;
    _probeBufferFrames = 0;
    _probeLastPosition.reset();
}

bool WindowsBackend::FillProbe()
{
    UINT32 padding{};
    HRESULT result = _probeClient->GetCurrentPadding(&padding);
    if (FAILED(result)) {
        LOG(Trace, "AudioStall: GetCurrentPadding failed. HRESULT: {:#x}", result);
        OnAudioFailure(result);
        return false;
    }

    const UINT32 available = _probeBufferFrames > padding ? _probeBufferFrames - padding : 0;
    if (available == 0) {
        return true;
    }

    BYTE *data{};
    result = _probeRender->GetBuffer(available, &data);
    if (FAILED(result)) {
        LOG(Trace, "AudioStall: GetBuffer failed. HRESULT: {:#x}", result);
        OnAudioFailure(result);
        return false;
    }

    result = _probeRender->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(result)) {
        LOG(Trace, "AudioStall: ReleaseBuffer failed. HRESULT: {:#x}", result);
        OnAudioFailure(result);
        return false;
    }
    return true;
}

std::optional<bool> WindowsBackend::TickProbe()
{
    if (!FillProbe() || !_probeClock) {
        StopProbe();
        return std::nullopt;
    }

    UINT64 position{};
    const HRESULT result = _probeClock->GetPosition(&position, nullptr);
    if (FAILED(result)) {
        LOG(Trace, "AudioStall: IAudioClock::GetPosition failed. HRESULT: {:#x}", result);
        StopProbe();
        OnAudioFailure(result);
        return std::nullopt;
    }

    if (!_probeLastPosition.has_value()) {
        _probeLastPosition = position;
        return std::nullopt;
    }

    const bool advancing = position > *_probeLastPosition;
    _probeLastPosition = position;
    return advancing;
}

} // namespace Core::AudioStall
