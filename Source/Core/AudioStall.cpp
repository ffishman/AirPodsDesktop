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

#include "AudioStall.h"

#include <condition_variable>
#include <future>
#include <thread>

#include <QMetaObject>

#include "../Logger.h"

using namespace std::chrono_literals;

namespace Core::AudioStall {
namespace {

// How long shutdown waits for a worker that may be blocked in a driver call before leaving it
// detached. The worker owns shared state only, so detaching it is safe.
constexpr auto kWorkerJoinTimeout = 3s;

const char *ProbeText(const std::optional<bool> &probe)
{
    if (!probe.has_value()) {
        return "n/a";
    }
    return *probe ? "advancing" : "stuck";
}

const char *ModeText(Mode mode)
{
    switch (mode) {
    case Mode::Disabled:
        return "disabled";
    case Mode::Observe:
        return "observe";
    case Mode::Recover:
        return "recover";
    }
    return "unknown";
}

} // namespace

struct Controller::Worker {
    std::stop_source stop;
    std::future<void> finished;
    std::thread thread;
};

Controller::State::State(std::shared_ptr<Backend> backend, Config config)
    : backend{std::move(backend)}, config{config}, detector{config.detector}, budget{config.budget}
{
}

std::vector<Notice> Controller::State::RunStep(Clock::time_point now, std::stop_token stopToken)
{
    std::vector<Notice> notices;

    const auto currentMode = mode.load();
    if (currentMode == Mode::Disabled) {
        return notices;
    }

    const Sample sample = backend->Poll();

    LOG(Trace, "AudioStall: eligible={} sessions={} probe={} peak={:.6f}", sample.eligible,
        sample.sessionsActive, ProbeText(sample.probeAdvancing), sample.endpointPeak);

    if (!lastSample.has_value() || lastSample->eligible != sample.eligible ||
        lastSample->sessionsActive != sample.sessionsActive ||
        lastSample->probeAdvancing != sample.probeAdvancing)
    {
        LOG(Info, "AudioStall: state changed. eligible={} sessions={} probe={} peak={:.6f}",
            sample.eligible, sample.sessionsActive, ProbeText(sample.probeAdvancing),
            sample.endpointPeak);
    }
    lastSample = sample;

    const auto event = detector.Feed(now, sample);
    switch (event) {
    case Event::SilenceSuspected:
        LOG(Info,
            "AudioStall: active sessions but the endpoint peak stayed at zero for {}ms. "
            "probe={} (weak evidence, no action)",
            detector.GetConfig().silenceGrace.count(), ProbeText(sample.probeAdvancing));
        break;
    case Event::StallDetected:
        LOG(Warn,
            "AudioStall: stall detected. The probe stream was not consumed for {}ms while "
            "sessions were active. peak={:.6f} mode={}",
            detector.GetConfig().stallGrace.count(), sample.endpointPeak, ModeText(currentMode));
        if (currentMode == Mode::Observe) {
            notices.push_back(Notice::StallObserved);
        }
        break;
    case Event::StallCleared:
        LOG(Info, "AudioStall: the stream runs again without intervention.");
        break;
    case Event::None:
        break;
    }

    if (verifyDeadline.has_value()) {
        if (sample.eligible && sample.sessionsActive && sample.probeAdvancing == true) {
            LOG(Info, "AudioStall: recovery verified, the stream runs again.");
            verifyDeadline.reset();
            notices.push_back(Notice::RecoverySucceeded);
        }
        else if (now >= *verifyDeadline) {
            LOG(Warn, "AudioStall: recovery could not be verified within {}ms.",
                config.verifyTimeout.count());
            verifyDeadline.reset();
        }
    }

    if (currentMode == Mode::Recover && detector.IsStalled() && sample.eligible &&
        sample.sessionsActive)
    {
        if (budget.TryConsume(now)) {
            gaveUpReported = false;
            LOG(Warn, "AudioStall: disconnecting and reconnecting the Bluetooth audio device.");
            notices.push_back(Notice::RecoveryStarted);

            const bool accepted = backend->Recover(stopToken);
            detector.Reset();
            lastSample.reset();

            if (accepted) {
                verifyDeadline = now + config.verifyTimeout;
            }
            else {
                LOG(Warn, "AudioStall: Windows did not accept the reconnect request.");
                verifyDeadline.reset();
                notices.push_back(Notice::RecoveryFailed);
            }
        }
        else if (budget.IsExhausted(now) && !gaveUpReported) {
            gaveUpReported = true;
            LOG(Warn, "AudioStall: giving up for now, {} attempts within {}ms.",
                config.budget.maxAttempts, config.budget.window.count());
            notices.push_back(Notice::RecoveryGaveUp);
        }
    }

    return notices;
}

Controller::Controller(std::shared_ptr<Backend> backend) : Controller{std::move(backend), Config{}}
{
}

Controller::Controller(std::shared_ptr<Backend> backend, Config config)
    : _state{std::make_shared<State>(std::move(backend), config)}
{
    std::lock_guard<std::mutex> lock{_link->mutex};
    _link->controller = this;
}

Controller::~Controller()
{
    {
        std::lock_guard<std::mutex> lock{_link->mutex};
        _link->controller = nullptr;
    }
    StopWorker();
}

void Controller::SetMode(Mode mode)
{
    const auto oldMode = _state->mode.exchange(mode);
    if (oldMode == mode) {
        return;
    }

    LOG(Info, "AudioStall: mode {} -> {}", ModeText(oldMode), ModeText(mode));

    if (_state->config.interval == 0ms) {
        return;
    }

    if (mode == Mode::Disabled) {
        StopWorker();
    }
    else if (oldMode == Mode::Disabled) {
        StartWorker();
    }
}

Mode Controller::GetMode() const
{
    return _state->mode.load();
}

void Controller::Step(Clock::time_point now)
{
    for (const auto notice : _state->RunStep(now, std::stop_token{})) {
        emit NoticeRaised(notice);
    }
}

void Controller::StartWorker()
{
    StopWorker();

    auto worker = std::make_shared<Worker>();
    std::promise<void> finished;
    worker->finished = finished.get_future();

    // The thread only holds shared state and its own copies, so it stays valid if shutdown has to
    // detach it while it is blocked inside a driver call.
    worker->thread =
        std::thread{[state = _state, link = _link, stopToken = worker->stop.get_token(),
                     finished = std::move(finished)]() mutable {
            state->backend->ThreadAttach();
            state->detector.Reset();
            state->budget.Reset();
            state->verifyDeadline.reset();
            state->gaveUpReported = false;
            state->lastSample.reset();

            std::mutex waitMutex;
            std::condition_variable_any wake;

            while (!stopToken.stop_requested()) {
                const auto notices = state->RunStep(Clock::now(), stopToken);
                if (!notices.empty()) {
                    std::lock_guard<std::mutex> lock{link->mutex};
                    if (link->controller != nullptr) {
                        for (const auto notice : notices) {
                            QMetaObject::invokeMethod(
                                link->controller,
                                [controller = link->controller, notice] {
                                    emit controller->NoticeRaised(notice);
                                },
                                Qt::QueuedConnection);
                        }
                    }
                }

                std::unique_lock<std::mutex> lock{waitMutex};
                wake.wait_for(lock, stopToken, state->config.interval, [] { return false; });
            }

            state->backend->ThreadDetach();
            finished.set_value();
        }};

    _worker = std::move(worker);
}

void Controller::StopWorker()
{
    if (!_worker) {
        return;
    }

    auto worker = std::move(_worker);
    worker->stop.request_stop();

    if (worker->finished.wait_for(kWorkerJoinTimeout) == std::future_status::ready) {
        worker->thread.join();
    }
    else {
        LOG(Warn, "AudioStall: worker did not stop in time, detaching it.");
        worker->thread.detach();

        // The detached thread keeps the old state. Give a future worker a fresh one so the two
        // never share the detector.
        auto fresh = std::make_shared<State>(_state->backend, _state->config);
        fresh->mode = _state->mode.load();
        _state = std::move(fresh);
    }
}

} // namespace Core::AudioStall
