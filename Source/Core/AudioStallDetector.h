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

// Pure decision logic for Bluetooth audio stall recovery. No platform calls, no Qt.
//
// The failure being handled: Windows keeps reporting the Bluetooth headset as connected and as the
// default output, but the A2DP stream never (re)starts, so every application renders into a sink
// that never consumes audio. Toggling the connection off and on fixes it.
//
// The platform layer feeds one `Sample` per tick:
//
//   - `eligible`        the default output is a Bluetooth audio endpoint
//   - `sessionsActive`  some other process has an active audio session on it
//   - `probeAdvancing`  our own silent probe stream is being consumed by the audio engine
//                       (`std::nullopt` while the probe has no baseline or is unavailable)
//   - `endpointPeak`    the endpoint peak meter, used only as weak evidence for the log
//
// Only the probe counts as strong evidence. A zero peak alone is ordinary: a paused video or a
// muted call keeps a session active while rendering silence.

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace Core::AudioStall {

using Clock = std::chrono::steady_clock;

struct Sample {
    bool eligible{};
    bool sessionsActive{};
    std::optional<bool> probeAdvancing;
    float endpointPeak{};
};

enum class Event : uint32_t {
    None,
    SilenceSuspected, // weak evidence only, log it once per episode
    StallDetected,    // strong evidence held for `stallGrace`
    StallCleared,     // the probe advances again after a detected stall
};

class Detector
{
public:
    struct Config {
        // The first second or two after a Bluetooth stream starts legitimately consumes nothing
        // while the A2DP link spins up.
        std::chrono::milliseconds stallGrace{5'000};
        std::chrono::milliseconds silenceGrace{10'000};
        float silencePeak{1e-6f};
    };

    Detector() = default;
    explicit Detector(Config config) : _config{config} {}

    Event Feed(Clock::time_point now, const Sample &sample);

    // Forget everything, for example after a recovery attempt or when the endpoint changed.
    void Reset();

    bool IsStalled() const
    {
        return _stalled;
    }

    const Config &GetConfig() const
    {
        return _config;
    }

private:
    Config _config;
    std::optional<Clock::time_point> _stallSince;
    std::optional<Clock::time_point> _silenceSince;
    bool _stalled{};
    bool _silenceReported{};
};

// Rate limit for recovery attempts, so a device that keeps failing (or that was deliberately moved
// to another host) does not get its connection toggled forever.
class RecoveryBudget
{
public:
    struct Config {
        std::chrono::milliseconds cooldown{30'000};
        uint32_t maxAttempts{3};
        std::chrono::milliseconds window{10 * 60'000};
    };

    RecoveryBudget() = default;
    explicit RecoveryBudget(Config config) : _config{config} {}

    // Records an attempt and returns true when one is allowed now.
    bool TryConsume(Clock::time_point now);

    // True when attempts are blocked by `maxAttempts` (as opposed to the cooldown).
    bool IsExhausted(Clock::time_point now);

    void Reset()
    {
        _attempts.clear();
    }

private:
    Config _config;
    std::deque<Clock::time_point> _attempts;

    void Expire(Clock::time_point now);
};

} // namespace Core::AudioStall
