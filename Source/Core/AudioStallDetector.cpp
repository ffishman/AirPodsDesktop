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

#include "AudioStallDetector.h"

namespace Core::AudioStall {

Event Detector::Feed(Clock::time_point now, const Sample &sample)
{
    if (!sample.eligible || !sample.sessionsActive) {
        // Nothing is playing, so there is nothing to judge. A detected stall stays detected: it is
        // only cleared by evidence that the stream runs again, or by `Reset`.
        _stallSince.reset();
        _silenceSince.reset();
        _silenceReported = false;
        return Event::None;
    }

    // Weak evidence, tracked independently of the probe so the log can show whether a real stall
    // was also visible on the peak meter, and whether the probe missed one.
    Event silenceEvent = Event::None;
    if (sample.endpointPeak <= _config.silencePeak) {
        if (!_silenceSince.has_value()) {
            _silenceSince = now;
        }
        if (!_silenceReported && now - *_silenceSince >= _config.silenceGrace) {
            _silenceReported = true;
            silenceEvent = Event::SilenceSuspected;
        }
    }
    else {
        _silenceSince.reset();
        _silenceReported = false;
    }

    if (sample.probeAdvancing == false) {
        if (_stalled) {
            return silenceEvent;
        }
        if (!_stallSince.has_value()) {
            _stallSince = now;
        }
        if (now - *_stallSince >= _config.stallGrace) {
            _stalled = true;
            return Event::StallDetected;
        }
        return silenceEvent;
    }

    _stallSince.reset();

    if (sample.probeAdvancing == true && _stalled) {
        _stalled = false;
        return Event::StallCleared;
    }

    return silenceEvent;
}

void Detector::Reset()
{
    _stallSince.reset();
    _silenceSince.reset();
    _stalled = false;
    _silenceReported = false;
}

void RecoveryBudget::Expire(Clock::time_point now)
{
    while (!_attempts.empty() && now - _attempts.front() >= _config.window) {
        _attempts.pop_front();
    }
}

bool RecoveryBudget::TryConsume(Clock::time_point now)
{
    Expire(now);

    if (!_attempts.empty() && now - _attempts.back() < _config.cooldown) {
        return false;
    }
    if (_attempts.size() >= _config.maxAttempts) {
        return false;
    }

    _attempts.push_back(now);
    return true;
}

bool RecoveryBudget::IsExhausted(Clock::time_point now)
{
    Expire(now);
    return _attempts.size() >= _config.maxAttempts;
}

} // namespace Core::AudioStall
