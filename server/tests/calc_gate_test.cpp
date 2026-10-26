#include "calc_gate.hpp"
#include "config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

using namespace rfmap;
using namespace std::chrono_literals;

TEST(CalcGateLimits, AutomaticLimitsKeepAQuarterOfTheRequestThreadsFree) {
    const Config::Calc automatic;
    // One calculation per core ...
    CalcGate::Limits l = CalcGate::limitsFor(automatic, 16, 4);
    EXPECT_EQ(l.concurrency, 4);
    EXPECT_EQ(l.queue, 4);
    EXPECT_EQ(l.wait, 10000ms);
    // ... at most half the request threads, with at most a quarter of them waiting ...
    l = CalcGate::limitsFor(automatic, 16, 64);
    EXPECT_EQ(l.concurrency, 8);
    EXPECT_EQ(l.queue, 4);
    // ... and never none, even when the core count is unknown (0).
    EXPECT_EQ(CalcGate::limitsFor(automatic, 2, 0).concurrency, 1);
    // On two request threads nobody waits: one thread always stays for other requests.
    EXPECT_EQ(CalcGate::limitsFor(automatic, 2, 8).queue, 0);
}

TEST(CalcGateLimits, ConfiguredLimitsWin) {
    Config::Calc set;
    set.concurrency = 3;
    set.queue = 6;
    set.waitMs = 0;
    const CalcGate::Limits l = CalcGate::limitsFor(set, 16, 4);
    EXPECT_EQ(l.concurrency, 3);
    EXPECT_EQ(l.queue, 6);
    EXPECT_EQ(l.wait, 0ms);
}

TEST(CalcGateLimits, ConfigReadsAndChecksTheGate) {
    const Config c = Config::fromJson({{"calc", {{"concurrency", 2}, {"queue", 3}, {"waitMs", 500}}}});
    EXPECT_EQ(c.calc.concurrency, 2);
    EXPECT_EQ(c.calc.queue, 3);
    EXPECT_EQ(c.calc.waitMs, 500);
    EXPECT_EQ(Config::load({}, {{"RFMAP__CALC__WAITMS", "1500"}}).calc.waitMs, 1500);
    EXPECT_THROW((void)Config::fromJson({{"calc", {{"queue", -1}}}}), std::invalid_argument);
    EXPECT_THROW((void)Config::fromJson({{"calc", {{"waitMs", -5}}}}), std::invalid_argument);
    EXPECT_THROW(CalcGate({0, 1, 1ms}), std::invalid_argument);
}

TEST(CalcGateLimits, BusyGateAnswers503WithRetryAfter) {
    CalcGate gate({1, 0, 0ms});
    const CalcGate::Permit held = gate.enter();
    try {
        (void)gate.run([] { return 1; });
        ADD_FAILURE() << "expected 503";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.status(), 503);
        EXPECT_EQ(e.body().at("retryAfterS").get<long long>(), 1);
        EXPECT_NE(std::string(e.what()).find("calculations"), std::string::npos);
    }
    EXPECT_EQ(gate.stats().rejected, 1u);
}

TEST(DatabaseConfig, ReadsAndChecksTheTimeouts) {
    const Config defaults = Config::fromJson(json::object());
    EXPECT_EQ(defaults.database.leaseTimeoutMs, 10000);
    EXPECT_EQ(defaults.database.statementTimeoutMs, 60000);
    const Config c = Config::load({}, {{"RFMAP__DATABASE__LEASETIMEOUTMS", "250"},
                                       {"RFMAP__DATABASE__STATEMENTTIMEOUTMS", "0"}});
    EXPECT_EQ(c.database.leaseTimeoutMs, 250);
    EXPECT_EQ(c.database.statementTimeoutMs, 0);
    EXPECT_THROW((void)Config::fromJson({{"database", {{"leaseTimeoutMs", -1}}}}), std::invalid_argument);
    EXPECT_THROW((void)Config::fromJson({{"database", {{"statementTimeoutMs", -1}}}}), std::invalid_argument);
}
