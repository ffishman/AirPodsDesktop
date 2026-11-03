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

#include "AudioStall.h"
#include "OS/Windows.h"

#include <chrono>
#include <optional>

#include <QString>

#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

namespace Core::AudioStall {

// Watches the default multimedia render endpoint when it is a Bluetooth audio endpoint.
//
// Evidence comes from a probe: while another process has an active session on the endpoint, this
// backend keeps its own shared-mode stream open on it and writes silence. If the audio engine is
// really pumping the endpoint, the probe's `IAudioClock` position advances between polls. If the
// Bluetooth stream is stuck, the probe buffer stays full and the position stands still, whatever
// the other applications are doing. The probe is closed as soon as no other session is active, so
// it never keeps the headset awake on its own.
class WindowsBackend final : public Backend
{
public:
    WindowsBackend() = default;
    ~WindowsBackend() override = default;

    void ThreadAttach() override;
    void ThreadDetach() override;

    Sample Poll() override;
    bool Recover(std::stop_token stopToken) override;

private:
    // Time between the disconnect and the reconnect request. The Settings app toggle that fixes the
    // stall by hand is never faster than this.
    static constexpr std::chrono::milliseconds kReconnectDelay{2'000};
    // Probe buffer. Must comfortably exceed the poll interval so a healthy probe never underruns.
    static constexpr REFERENCE_TIME kProbeBufferDuration{10'000'000}; // 1 s in 100 ns units
    static constexpr std::chrono::seconds kProbeRetryDelay{10};

    bool _comInitialized{};
    OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> _enumerator;

    // Current default endpoint.
    QString _endpointId;
    QString _containerId;
    bool _bluetooth{};
    OS::Windows::Com::UniquePtr<IMMDevice> _device;
    OS::Windows::Com::UniquePtr<IAudioSessionManager2> _sessionManager;
    OS::Windows::Com::UniquePtr<IAudioMeterInformation> _meter;

    // Probe stream on that endpoint.
    OS::Windows::Com::UniquePtr<IAudioClient> _probeClient;
    OS::Windows::Com::UniquePtr<IAudioRenderClient> _probeRender;
    OS::Windows::Com::UniquePtr<IAudioClock> _probeClock;
    UINT32 _probeBufferFrames{};
    std::optional<UINT64> _probeLastPosition;
    std::optional<std::chrono::steady_clock::time_point> _probeRetryAfter;

    bool ResolveDefaultEndpoint();
    void ReleaseEndpoint();
    void OnAudioFailure(HRESULT result);

    bool AnyForeignSessionActive();
    float ReadPeak();

    bool StartProbe();
    void StopProbe();
    bool FillProbe();
    std::optional<bool> TickProbe();
};

} // namespace Core::AudioStall
