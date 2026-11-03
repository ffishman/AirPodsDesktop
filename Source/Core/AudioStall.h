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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <QObject>

#include "AudioStallDetector.h"

namespace Core::AudioStall {

enum class Mode : uint32_t {
    Disabled,
    Observe, // detect and log, never touch the connection
    Recover, // detect, then disconnect and reconnect the Bluetooth audio device
};

enum class Notice : uint32_t {
    StallObserved,     // Observe mode: a stall was detected
    RecoveryStarted,   // Recover mode: the connection is being toggled
    RecoverySucceeded, // the stream runs again after a recovery
    RecoveryFailed,    // Windows rejected the disconnect/reconnect requests
    RecoveryGaveUp,    // the attempt budget is exhausted for now
};

// Platform side. Every method except the constructor and destructor runs on the controller's
// worker thread, between `ThreadAttach` and `ThreadDetach`.
class Backend
{
public:
    virtual ~Backend() = default;

    virtual void ThreadAttach() {}
    virtual void ThreadDetach() {}

    virtual Sample Poll() = 0;

    // Disconnects and reconnects the Bluetooth audio device behind the current default output.
    // Returns true when Windows accepted the reconnect request.
    virtual bool Recover(std::stop_token stopToken) = 0;
};

class NullBackend final : public Backend
{
public:
    Sample Poll() override
    {
        return {};
    }
    bool Recover(std::stop_token) override
    {
        return false;
    }
};

class Controller final : public QObject
{
    Q_OBJECT

public:
    struct Config {
        Detector::Config detector{};
        RecoveryBudget::Config budget{};
        // How long a recovery may take to show a running stream before it is reported as
        // unverified. Has to exceed the reconnect itself plus `detector.stallGrace`.
        std::chrono::milliseconds verifyTimeout{20'000};
        // Poll interval of the worker thread. Zero means no worker: the owner drives `Step`.
        std::chrono::milliseconds interval{500};
    };

    explicit Controller(std::shared_ptr<Backend> backend);
    Controller(std::shared_ptr<Backend> backend, Config config);
    ~Controller() override;

    void SetMode(Mode mode);
    Mode GetMode() const;

    // Runs one poll/decide/act iteration on the calling thread and emits the resulting notices
    // directly. Meant for tests with `Config::interval == 0`; never call it while a worker runs.
    void Step(Clock::time_point now);

Q_SIGNALS:
    void NoticeRaised(Core::AudioStall::Notice notice);

private:
    struct Link {
        std::mutex mutex;
        Controller *controller{};
    };

    // Everything the worker touches. Shared, so a worker stuck inside a driver call can be
    // detached at shutdown without dangling.
    struct State {
        explicit State(std::shared_ptr<Backend> backend, Config config);

        std::shared_ptr<Backend> backend;
        Config config;
        std::atomic<Mode> mode{Mode::Disabled};

        // Worker-thread only.
        Detector detector;
        RecoveryBudget budget;
        std::optional<Clock::time_point> verifyDeadline;
        bool gaveUpReported{};
        std::optional<Sample> lastSample;

        std::vector<Notice> RunStep(Clock::time_point now, std::stop_token stopToken);
    };

    struct Worker;

    std::shared_ptr<State> _state;
    std::shared_ptr<Link> _link{std::make_shared<Link>()};
    std::shared_ptr<Worker> _worker;

    void StartWorker();
    void StopWorker();
};

} // namespace Core::AudioStall
