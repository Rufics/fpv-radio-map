#include "auth.hpp"
#include "config.hpp"
#include "pin_gate.hpp"
#include "security.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace rfmap;
using namespace std::chrono_literals;
using rfmap::test::FakeClock;

namespace {

/// Holds the gate's slots and parks callers in its queue, so a test can see what a full gate does.
class GateTest : public ::testing::Test {
protected:
    /// Blocks until `waiting` callers wait for a slot.
    void awaitWaiting(const PinGate& gate, int waiting) const {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (gate.stats().waiting < waiting && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        ASSERT_EQ(gate.stats().waiting, waiting);
    }

    /// Expects `enter()` to be turned away with 503; returns its retryAfterS.
    static long long expectBusy(PinGate& gate) {
        try {
            const PinGate::Permit permit = gate.enter();
        } catch (const HttpError& e) {
            EXPECT_EQ(e.status(), 503) << e.what();
            return e.body().at("retryAfterS").get<long long>();
        }
        ADD_FAILURE() << "expected 503";
        return 0;
    }
};

}  // namespace

TEST(PinGateLimits, AutomaticLimitsKeepMostRequestThreadsFree) {
    const Config::Auth automatic;
    // Half the cores ...
    PinGate::Limits l = PinGate::limitsFor(automatic, 16, 4);
    EXPECT_EQ(l.concurrency, 2);
    EXPECT_EQ(l.queue, 2);
    EXPECT_EQ(l.wait, 2000ms);
    // ... but at most a quarter of the request threads ...
    EXPECT_EQ(PinGate::limitsFor(automatic, 8, 32).concurrency, 2);
    // ... and never none, even when the core count is unknown (0).
    EXPECT_EQ(PinGate::limitsFor(automatic, 2, 0).concurrency, 1);

    Config::Auth set;
    set.pinConcurrency = 3;
    set.pinQueue = 5;
    set.pinWaitMs = 0;
    l = PinGate::limitsFor(set, 16, 4);
    EXPECT_EQ(l.concurrency, 3);
    EXPECT_EQ(l.queue, 5);
    EXPECT_EQ(l.wait, 0ms);
}

TEST(PinGateLimits, ConfigReadsAndChecksTheGate) {
    const Config c = Config::fromJson({{"auth", {{"pinConcurrency", 2}, {"pinQueue", 4}, {"pinWaitMs", 500}}}});
    EXPECT_EQ(c.auth.pinConcurrency, 2);
    EXPECT_EQ(c.auth.pinQueue, 4);
    EXPECT_EQ(c.auth.pinWaitMs, 500);
    EXPECT_THROW((void)Config::fromJson({{"auth", {{"pinQueue", -1}}}}), std::invalid_argument);
    EXPECT_THROW((void)Config::fromJson({{"auth", {{"pinWaitMs", -5}}}}), std::invalid_argument);
    EXPECT_THROW(PinGate({0, 1, 1ms}), std::invalid_argument);
}

TEST_F(GateTest, AdmitsUpToItsConcurrencyAndFreesSlotsOnScopeExit) {
    PinGate gate({2, 0, 0ms});
    {
        const PinGate::Permit a = gate.enter();
        const PinGate::Permit b = gate.enter();
        EXPECT_EQ(gate.stats().active, 2);
        EXPECT_EQ(expectBusy(gate), 1);  // no wait configured: retry after a second
    }
    EXPECT_EQ(gate.stats().active, 0);
    EXPECT_EQ(gate.stats().rejected, 1u);
    EXPECT_EQ(gate.run([] { return 42; }), 42);
}

TEST_F(GateTest, SlotFreedWhileWaitingAdmitsTheWaiter) {
    PinGate gate({1, 1, 10s});
    std::optional<PinGate::Permit> held(gate.enter());
    std::atomic<bool> admitted{false};
    std::jthread waiter([&gate, &admitted] {
        const PinGate::Permit p = gate.enter();
        admitted = true;
    });
    awaitWaiting(gate, 1);
    EXPECT_FALSE(admitted);
    held.reset();
    waiter.join();
    EXPECT_TRUE(admitted);
    EXPECT_EQ(gate.stats().rejected, 0u);
}

TEST_F(GateTest, FullQueueIsTurnedAwayAtOnce) {
    PinGate gate({1, 1, 10s});
    std::optional<PinGate::Permit> held(gate.enter());
    std::jthread waiter([&gate] { const PinGate::Permit p = gate.enter(); });
    awaitWaiting(gate, 1);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(expectBusy(gate), 10);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);  // did not wait the 10 s
    held.reset();
}

TEST_F(GateTest, WaiterGivesUpAfterTheWait) {
    PinGate gate({1, 1, 50ms});
    const PinGate::Permit held = gate.enter();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(expectBusy(gate), 1);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);
    EXPECT_EQ(gate.stats().waiting, 0);
}

TEST(PinGateAuth, BusyGateAnswers503AndDoesNotCountAnAttempt) {
    FakeClock clock;
    MemoryAuthStore store;
    const PinHasher pins{PinHasher::Cost::Interactive};
    PinGate gate({1, 0, 0ms});
    AuthService auth{store, pins, gate, clock, {2, std::chrono::minutes(15), std::chrono::hours(24)}};
    const auto owner = AuthService::newOwnerKey();
    store.addMission("m1", owner.hash);
    const auto link = auth.createLink("m1", owner.plain, Role::View, "123456", "crew");
    {
        const PinGate::Permit busy = gate.enter();
        for (int i = 0; i < 3; ++i) {
            try {
                (void)auth.openLink(link.token, "000000");
                ADD_FAILURE() << "expected 503";
            } catch (const HttpError& e) {
                EXPECT_EQ(e.status(), 503);
            }
        }
    }
    // Three turned-away guesses against a limit of two: the link is not paused and still opens.
    EXPECT_EQ(store.links("m1").at(0).failedAttempts, 0);
    EXPECT_EQ(auth.openLink(link.token, "123456").missionId, "m1");
}
