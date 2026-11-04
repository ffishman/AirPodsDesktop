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

#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "Source/Core/AudioStall.h"
#include "Source/Core/AudioStallDetector.h"

using namespace std::chrono_literals;

namespace {

using Core::AudioStall::Clock;
using Core::AudioStall::Controller;
using Core::AudioStall::Detector;
using Core::AudioStall::Event;
using Core::AudioStall::Mode;
using Core::AudioStall::Notice;
using Core::AudioStall::RecoveryBudget;
using Core::AudioStall::Sample;

const Clock::time_point kT0{};

Sample Idle()
{
    return Sample{true, false, std::nullopt, 0.f};
}

Sample Stuck()
{
    return Sample{true, true, false, 0.f};
}

Sample Running(float peak = 0.2f)
{
    return Sample{true, true, true, peak};
}

class FakeBackend final : public Core::AudioStall::Backend
{
public:
    Sample next{};
    // Applied to `next` when `Recover` runs, to model a recovery that does or does not work.
    std::optional<Sample> afterRecover;
    bool accept{true};
    std::atomic<int> polls{};
    std::atomic<int> recovers{};
    std::atomic<int> attaches{};
    std::atomic<int> detaches{};

    void ThreadAttach() override
    {
        ++attaches;
    }

    void ThreadDetach() override
    {
        ++detaches;
    }

    Sample Poll() override
    {
        ++polls;
        return next;
    }

    bool Recover(std::stop_token) override
    {
        ++recovers;
        if (afterRecover.has_value()) {
            next = *afterRecover;
        }
        return accept;
    }
};

Controller::Config ManualConfig()
{
    Controller::Config config;
    config.interval = 0ms;
    config.detector.stallGrace = 5s;
    config.detector.silenceGrace = 10s;
    config.budget.cooldown = 30s;
    config.budget.maxAttempts = 3;
    config.budget.window = 10min;
    config.verifyTimeout = 20s;
    return config;
}

struct Harness {
    std::shared_ptr<FakeBackend> backend{std::make_shared<FakeBackend>()};
    Controller controller{backend, ManualConfig()};
    std::vector<Notice> notices;

    Harness()
    {
        QObject::connect(&controller, &Controller::NoticeRaised, [this](Notice notice) {
            notices.push_back(notice);
        });
    }

    // Steps every 500ms from `from` to `to`, inclusive.
    void Run(Clock::time_point from, Clock::time_point to)
    {
        for (auto t = from; t <= to; t += 500ms) {
            controller.Step(t);
        }
    }

    int Count(Notice notice) const
    {
        return static_cast<int>(std::count(notices.begin(), notices.end(), notice));
    }
};

} // namespace

class AudioStallTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Detector
    void idleSamplesNeverReport();
    void stallNeedsTheFullGracePeriod();
    void stallIsReportedOncePerEpisode();
    void advancingProbeClearsTheStall();
    void probeWithoutBaselineIsNotEvidence();
    void sessionsStoppingRestartsTheGracePeriod();
    void silenceIsWeakEvidenceReportedOnce();
    void resetForgetsADetectedStall();

    // Budget
    void budgetEnforcesCooldown();
    void budgetEnforcesAttemptLimitWithinWindow();

    // Controller
    void disabledModeDoesNotPoll();
    void observeModeReportsButNeverRecovers();
    void recoverModeRecoversAndVerifies();
    void rejectedReconnectReportsFailure();
    void persistentStallGivesUpAfterTheBudget();
    void healthyStreamNeverTriggersRecovery();
    void workerThreadPollsAndDeliversNotices();
    void manualRecoveryWorksInObserveMode();
    void manualRecoveryNeedsABluetoothOutput();
    void manualRecoveryBypassesTheBudget();
    void isStalledFollowsTheDetector();
};

void AudioStallTests::idleSamplesNeverReport()
{
    Detector detector;
    for (auto t = kT0; t <= kT0 + 60s; t += 500ms) {
        QCOMPARE(detector.Feed(t, Idle()), Event::None);
        QCOMPARE(detector.Feed(t, Sample{false, true, false, 0.f}), Event::None);
    }
    QVERIFY(!detector.IsStalled());
}

void AudioStallTests::stallNeedsTheFullGracePeriod()
{
    Detector detector{Detector::Config{5s, 10s, 1e-6f}};
    for (auto t = kT0; t < kT0 + 5s; t += 500ms) {
        QCOMPARE(detector.Feed(t, Stuck()), Event::None);
    }
    QCOMPARE(detector.Feed(kT0 + 5s, Stuck()), Event::StallDetected);
    QVERIFY(detector.IsStalled());
}

void AudioStallTests::stallIsReportedOncePerEpisode()
{
    Detector detector{Detector::Config{5s, 60s, 1e-6f}};
    int detected = 0;
    for (auto t = kT0; t <= kT0 + 30s; t += 500ms) {
        if (detector.Feed(t, Stuck()) == Event::StallDetected) {
            ++detected;
        }
    }
    QCOMPARE(detected, 1);
}

void AudioStallTests::advancingProbeClearsTheStall()
{
    Detector detector{Detector::Config{5s, 60s, 1e-6f}};
    detector.Feed(kT0, Stuck());
    QCOMPARE(detector.Feed(kT0 + 5s, Stuck()), Event::StallDetected);
    QCOMPARE(detector.Feed(kT0 + 6s, Running()), Event::StallCleared);
    QVERIFY(!detector.IsStalled());
    QCOMPARE(detector.Feed(kT0 + 7s, Running()), Event::None);
}

void AudioStallTests::probeWithoutBaselineIsNotEvidence()
{
    Detector detector{Detector::Config{5s, 60s, 1e-6f}};
    for (auto t = kT0; t <= kT0 + 30s; t += 500ms) {
        QCOMPARE(detector.Feed(t, Sample{true, true, std::nullopt, 0.f}), Event::None);
    }
    QVERIFY(!detector.IsStalled());
}

void AudioStallTests::sessionsStoppingRestartsTheGracePeriod()
{
    Detector detector{Detector::Config{5s, 60s, 1e-6f}};
    detector.Feed(kT0, Stuck());
    detector.Feed(kT0 + 4s, Stuck());
    detector.Feed(kT0 + 4500ms, Idle());
    // The clock starts over at 5s, so 9s is still inside the new grace period.
    QCOMPARE(detector.Feed(kT0 + 5s, Stuck()), Event::None);
    QCOMPARE(detector.Feed(kT0 + 9s, Stuck()), Event::None);
    QCOMPARE(detector.Feed(kT0 + 10s, Stuck()), Event::StallDetected);
}

void AudioStallTests::silenceIsWeakEvidenceReportedOnce()
{
    Detector detector{Detector::Config{5s, 10s, 1e-6f}};
    int suspected = 0;
    for (auto t = kT0; t <= kT0 + 60s; t += 500ms) {
        const auto event = detector.Feed(t, Running(0.f));
        QVERIFY(event != Event::StallDetected);
        if (event == Event::SilenceSuspected) {
            ++suspected;
        }
    }
    QCOMPARE(suspected, 1);
    QVERIFY(!detector.IsStalled());

    // A sound in between starts a new episode.
    QCOMPARE(detector.Feed(kT0 + 61s, Running(0.5f)), Event::None);
    detector.Feed(kT0 + 62s, Running(0.f));
    QCOMPARE(detector.Feed(kT0 + 72s, Running(0.f)), Event::SilenceSuspected);
}

void AudioStallTests::resetForgetsADetectedStall()
{
    Detector detector{Detector::Config{5s, 60s, 1e-6f}};
    detector.Feed(kT0, Stuck());
    detector.Feed(kT0 + 5s, Stuck());
    QVERIFY(detector.IsStalled());
    detector.Reset();
    QVERIFY(!detector.IsStalled());
    QCOMPARE(detector.Feed(kT0 + 6s, Stuck()), Event::None);
    QCOMPARE(detector.Feed(kT0 + 11s, Stuck()), Event::StallDetected);
}

void AudioStallTests::budgetEnforcesCooldown()
{
    RecoveryBudget budget{RecoveryBudget::Config{30s, 3, 10min}};
    QVERIFY(budget.TryConsume(kT0));
    QVERIFY(!budget.TryConsume(kT0 + 29s));
    QVERIFY(!budget.IsExhausted(kT0 + 29s));
    QVERIFY(budget.TryConsume(kT0 + 30s));
}

void AudioStallTests::budgetEnforcesAttemptLimitWithinWindow()
{
    RecoveryBudget budget{RecoveryBudget::Config{30s, 3, 10min}};
    QVERIFY(budget.TryConsume(kT0));
    QVERIFY(budget.TryConsume(kT0 + 1min));
    QVERIFY(budget.TryConsume(kT0 + 2min));
    QVERIFY(budget.IsExhausted(kT0 + 3min));
    QVERIFY(!budget.TryConsume(kT0 + 3min));
    // The first attempt leaves the window at 10min.
    QVERIFY(budget.TryConsume(kT0 + 10min));
}

void AudioStallTests::disabledModeDoesNotPoll()
{
    Harness h;
    h.backend->next = Stuck();
    h.Run(kT0, kT0 + 60s);
    QCOMPARE(h.backend->polls, 0);
    QVERIFY(h.notices.empty());
}

void AudioStallTests::observeModeReportsButNeverRecovers()
{
    Harness h;
    h.controller.SetMode(Mode::Observe);
    h.backend->next = Stuck();
    h.Run(kT0, kT0 + 5min);
    QCOMPARE(h.backend->recovers, 0);
    QCOMPARE(h.Count(Notice::StallObserved), 1);
    QCOMPARE(h.notices.size(), size_t{1});
}

void AudioStallTests::recoverModeRecoversAndVerifies()
{
    Harness h;
    h.controller.SetMode(Mode::Recover);
    h.backend->next = Stuck();
    h.backend->afterRecover = Running();

    h.Run(kT0, kT0 + 4500ms);
    QCOMPARE(h.backend->recovers, 0);

    h.Run(kT0 + 5s, kT0 + 10s);
    QCOMPARE(h.backend->recovers, 1);
    QCOMPARE(h.Count(Notice::RecoveryStarted), 1);
    QCOMPARE(h.Count(Notice::RecoverySucceeded), 1);
    QCOMPARE(h.Count(Notice::StallObserved), 0);

    h.Run(kT0 + 10500ms, kT0 + 5min);
    QCOMPARE(h.backend->recovers, 1);
    QCOMPARE(h.Count(Notice::RecoverySucceeded), 1);
}

void AudioStallTests::rejectedReconnectReportsFailure()
{
    Harness h;
    h.controller.SetMode(Mode::Recover);
    h.backend->next = Stuck();
    h.backend->accept = false;

    h.Run(kT0, kT0 + 5s);
    QCOMPARE(h.backend->recovers, 1);
    QCOMPARE(h.Count(Notice::RecoveryFailed), 1);
    QCOMPARE(h.Count(Notice::RecoverySucceeded), 0);
}

void AudioStallTests::persistentStallGivesUpAfterTheBudget()
{
    Harness h;
    h.controller.SetMode(Mode::Recover);
    h.backend->next = Stuck(); // stays stuck whatever Recover does

    h.Run(kT0, kT0 + 9min);
    QCOMPARE(h.backend->recovers, 3);
    QCOMPARE(h.Count(Notice::RecoveryStarted), 3);
    QCOMPARE(h.Count(Notice::RecoveryGaveUp), 1);
    QCOMPARE(h.Count(Notice::RecoverySucceeded), 0);
}

void AudioStallTests::healthyStreamNeverTriggersRecovery()
{
    Harness h;
    h.controller.SetMode(Mode::Recover);
    h.backend->next = Running(0.f); // a paused video: active session, silent, but consumed
    h.Run(kT0, kT0 + 10min);
    QCOMPARE(h.backend->recovers, 0);
    QVERIFY(h.notices.empty());
}

void AudioStallTests::workerThreadPollsAndDeliversNotices()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->next = Stuck();

    Controller::Config config;
    config.interval = 20ms;
    config.detector.stallGrace = 200ms;

    std::vector<Notice> notices;
    {
        Controller controller{backend, config};
        QObject::connect(&controller, &Controller::NoticeRaised, [&notices](Notice notice) {
            notices.push_back(notice);
        });

        controller.SetMode(Mode::Observe);
        QTRY_COMPARE_WITH_TIMEOUT(notices.size(), size_t{1}, 5000);
        QCOMPARE(notices.front(), Notice::StallObserved);
        QCOMPARE(backend->attaches.load(), 1);
        QCOMPARE(backend->recovers.load(), 0);

        controller.SetMode(Mode::Disabled);
        QCOMPARE(backend->detaches.load(), 1);
        const int pollsAfterStop = backend->polls.load();
        QTest::qWait(100);
        QCOMPARE(backend->polls.load(), pollsAfterStop);

        // Restarting and destroying while running must also stop the worker.
        controller.SetMode(Mode::Recover);
        QTRY_VERIFY_WITH_TIMEOUT(backend->attaches.load() == 2, 5000);
    }
    QCOMPARE(backend->detaches.load(), 2);
}

void AudioStallTests::manualRecoveryWorksInObserveMode()
{
    Harness h;
    h.controller.SetMode(Mode::Observe);
    h.backend->next = Stuck();
    h.backend->afterRecover = Running();

    h.Run(kT0, kT0 + 6s);
    QVERIFY(h.controller.IsStalled());
    QCOMPARE(h.backend->recovers.load(), 0);

    h.controller.RequestRecovery();
    h.controller.Step(kT0 + 6500ms);
    QCOMPARE(h.backend->recovers.load(), 1);
    QCOMPARE(h.Count(Notice::RecoveryStarted), 1);
    QVERIFY(!h.controller.IsStalled());

    h.controller.Step(kT0 + 7s);
    QCOMPARE(h.Count(Notice::RecoverySucceeded), 1);

    // The request is consumed once.
    h.Run(kT0 + 7500ms, kT0 + 2min);
    QCOMPARE(h.backend->recovers.load(), 1);
}

void AudioStallTests::manualRecoveryNeedsABluetoothOutput()
{
    Harness h;
    h.controller.SetMode(Mode::Observe);
    h.backend->next = Sample{false, true, false, 0.f};
    h.controller.RequestRecovery();
    h.controller.Step(kT0);
    QCOMPARE(h.backend->recovers.load(), 0);
    QVERIFY(h.notices.empty());
}

void AudioStallTests::manualRecoveryBypassesTheBudget()
{
    Harness h;
    h.controller.SetMode(Mode::Recover);
    h.backend->next = Stuck();

    h.Run(kT0, kT0 + 5s); // automatic attempt 1 at 5s
    QCOMPARE(h.backend->recovers.load(), 1);

    // Inside the 30s cooldown an automatic attempt is refused, a manual one is not.
    h.controller.RequestRecovery();
    h.controller.Step(kT0 + 6s);
    QCOMPARE(h.backend->recovers.load(), 2);
}

void AudioStallTests::isStalledFollowsTheDetector()
{
    Harness h;
    h.controller.SetMode(Mode::Observe);
    h.backend->next = Stuck();
    h.Run(kT0, kT0 + 4s);
    QVERIFY(!h.controller.IsStalled());
    h.Run(kT0 + 4500ms, kT0 + 6s);
    QVERIFY(h.controller.IsStalled());

    h.backend->next = Running();
    h.controller.Step(kT0 + 6500ms);
    QVERIFY(!h.controller.IsStalled());

    h.backend->next = Stuck();
    h.Run(kT0 + 7s, kT0 + 13s);
    QVERIFY(h.controller.IsStalled());
    h.controller.SetMode(Mode::Disabled);
    h.controller.Step(kT0 + 14s);
    QVERIFY(!h.controller.IsStalled());
}

QTEST_GUILESS_MAIN(AudioStallTests)

#include "AudioStallTests.moc"
