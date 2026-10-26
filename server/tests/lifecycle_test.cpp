#include "lifecycle.hpp"
#include "readiness.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace rfmap;
using namespace std::chrono_literals;

namespace {

/// Polls `done` for up to two seconds.
bool eventually(const std::atomic<bool>& done) {
    for (int i = 0; i < 200 && !done.load(); ++i) std::this_thread::sleep_for(10ms);
    return done.load();
}

}  // namespace

TEST(GracefulStopTest, DrainsThenStopsInOrder) {
    std::atomic<bool> drained{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> drainedBeforeStop{false};
    GracefulStop stop(
        100ms, [&drained] { drained = true; },
        [&stopped, &drained, &drainedBeforeStop] {
            drainedBeforeStop = drained.load();
            stopped = true;
        });
    EXPECT_FALSE(stop.requested());
    std::this_thread::sleep_for(120ms);
    EXPECT_FALSE(drained.load());  // nothing happens until asked

    const auto asked = std::chrono::steady_clock::now();
    stop.request();
    EXPECT_TRUE(stop.requested());
    EXPECT_TRUE(eventually(drained));
    EXPECT_TRUE(stop.draining());
    EXPECT_TRUE(eventually(stopped));
    EXPECT_TRUE(stop.stopped());
    EXPECT_TRUE(drainedBeforeStop.load());
    EXPECT_GE(std::chrono::steady_clock::now() - asked, 100ms);
}

TEST(GracefulStopTest, DestroyedDuringTheDrainNeverStops) {
    std::atomic<bool> drained{false};
    std::atomic<bool> stopped{false};
    {
        GracefulStop stop(
            10s, [&drained] { drained = true; }, [&stopped] { stopped = true; });
        stop.request();
        ASSERT_TRUE(eventually(drained));
    }
    EXPECT_FALSE(stopped.load());
}

TEST(GracefulStopTest, SecondRequestCutsTheDrainShort) {
    std::atomic<bool> stopped{false};
    GracefulStop stop(
        10s, [] {}, [&stopped] { stopped = true; });
    const auto asked = std::chrono::steady_clock::now();
    stop.request();
    for (int i = 0; i < 200 && !stop.draining(); ++i) std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(stop.draining());
    stop.request();
    EXPECT_TRUE(eventually(stopped));
    EXPECT_LT(std::chrono::steady_clock::now() - asked, 5s);
}

TEST(GracefulStopTest, SecondRequestRunsTheStopPhaseOnce) {
    std::atomic<int> stops{0};
    GracefulStop stop(
        0ms, [] {}, [&stops] { ++stops; });
    stop.request();
    stop.request();
    std::atomic<bool> done{false};
    for (int i = 0; i < 200 && !stop.stopped(); ++i) std::this_thread::sleep_for(10ms);
    done = stop.stopped();
    EXPECT_TRUE(done.load());
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(stops.load(), 1);
}

TEST(ReadinessChecksTest, RequiredChecksDecideInformationalOnesReport) {
    ReadinessChecks checks;
    checks.addRequired("database", [] { return CheckResult{true, "ok"}; });
    checks.addInformational("dem", [] { return CheckResult{false, "missing"}; });
    auto report = checks.run();
    EXPECT_TRUE(report.ready);
    EXPECT_TRUE(report.body.at("ready").get<bool>());
    EXPECT_EQ(report.body.at("checks").at("database").at("detail"), "ok");
    EXPECT_FALSE(report.body.at("checks").at("dem").at("ok").get<bool>());

    checks.addRequired("broken", [] { return CheckResult{false, "down"}; });
    report = checks.run();
    EXPECT_FALSE(report.ready);
}

TEST(ReadinessChecksTest, AThrowingCheckFailsWithItsMessage) {
    ReadinessChecks checks;
    checks.addRequired("probe", []() -> CheckResult { throw std::runtime_error("connection refused"); });
    const auto report = checks.run();
    EXPECT_FALSE(report.ready);
    EXPECT_EQ(report.body.at("checks").at("probe").at("detail"), "connection refused");
}

TEST(DatabaseProbeTest, ProbeUrlAddsClientSideTimeoutsUnlessTheOperatorSetThem) {
    EXPECT_EQ(DatabaseProbe::probeUrl("postgresql://u@h/db", 1000ms),
              "postgresql://u@h/db?connect_timeout=2&tcp_user_timeout=1000");
    EXPECT_EQ(DatabaseProbe::probeUrl("postgresql://u@h/db?sslmode=require", 2500ms),
              "postgresql://u@h/db?sslmode=require&connect_timeout=3&tcp_user_timeout=2500");
    EXPECT_EQ(DatabaseProbe::probeUrl("postgresql://u@h/db?connect_timeout=10", 1000ms),
              "postgresql://u@h/db?connect_timeout=10&tcp_user_timeout=1000");
    EXPECT_EQ(DatabaseProbe::probeUrl("host=h dbname=db", 1000ms),
              "host=h dbname=db connect_timeout=2 tcp_user_timeout=1000");
}

TEST(DatabaseProbeTest, BudgetIsConnectPlusStatementTimeout) {
    Logger log;
    log.setQuiet(true);
    const DatabaseProbe probe("postgresql://u@h/db", 500ms, log);
    EXPECT_EQ(probe.budget(), 2500ms);
}

TEST(DatabaseProbeTest, UnreachableDatabaseIsNotReady) {
    Logger log;
    log.setQuiet(true);
    // Nothing listens on this port; the probe must answer, not throw or hang.
    DatabaseProbe probe("postgresql://rfmap@127.0.0.1:1/none?connect_timeout=1", 500ms, log);
    const CheckResult r = probe.check();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.detail, "database unreachable");
}
