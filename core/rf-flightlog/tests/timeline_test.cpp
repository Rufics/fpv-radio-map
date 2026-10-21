// The even timeline: resampling without bridging gaps, energy, cell count, and the channels
// derived from GPS.

#include "flight_fixture.hpp"

#include <cmath>
#include <rfflightlog/timeline.hpp>

#include <gtest/gtest.h>

using namespace rfflightlog;

TEST(Resampler, InterpolatesAndKeepsGaps) {
    const Resampler r(1.0);
    const std::vector<double> t = {0.0, 1.0, 2.0, 5.0, 6.0};
    const std::vector<double> v = {0.0, 10.0, kNoValue, 50.0, 60.0};
    const auto out = r.resample(t, v, {-1.0, 0.0, 0.5, 1.5, 3.0, 5.5, 7.0});
    EXPECT_FALSE(hasValue(out[0]));  // before the first sample
    EXPECT_DOUBLE_EQ(out[1], 0.0);
    EXPECT_DOUBLE_EQ(out[2], 5.0);
    EXPECT_FALSE(hasValue(out[3]));  // next to a missing value
    EXPECT_FALSE(hasValue(out[4]));  // across a 3 s gap
    EXPECT_DOUBLE_EQ(out[5], 55.0);
    EXPECT_FALSE(hasValue(out[6]));  // after the last sample
}

TEST(EnergyMeter, IntegratesCurrentAndPower) {
    const std::vector<double> t = {0.0, 18.0, 36.0, 100.0};
    const EnergyMeter::Totals e = EnergyMeter(1000.0).integrate(t, {10.0, 10.0, 10.0, 10.0}, {10.0, 10.0, 10.0, 10.0});
    EXPECT_NEAR(e.usedMah[2], 100.0, 1e-9);
    EXPECT_NEAR(e.usedWh[2], 1.0, 1e-9);
    const EnergyMeter::Totals gap = EnergyMeter(30.0).integrate(t, {10.0, 10.0, 10.0, 10.0}, {10.0, 10.0, 10.0, 10.0});
    EXPECT_NEAR(gap.usedMah[3], 100.0, 1e-9) << "a 64 s gap adds nothing";
}

TEST(CellCounter, CountsSeriesCellsFromTheStartVoltage) {
    const CellCounter c(4.3);
    EXPECT_EQ(c.count({0.0}, {16.8}), 4);
    EXPECT_EQ(c.count({0.0}, {25.2}), 6);
    EXPECT_EQ(c.count({0.0}, {14.8}), 4);
    EXPECT_EQ(c.count({0.0}, {12.0}), 3);
    EXPECT_EQ(c.count({0.0}, {4.2}), 1);
    EXPECT_EQ(c.count({0.0, 10.0}, {kNoValue, 16.8}), 0) << "only the first seconds count";
    EXPECT_EQ(c.count({}, {}), 0);
    EXPECT_EQ(CellCounter(1e-9).count({0.0}, {16.8}), 0) << "a cell maximum near zero gives no count";
    EXPECT_EQ(c.count({0.0}, {1e12}), 0) << "nor does a voltage no pack has";
}

TEST(TimelineBuilder, BuildsTheDerivedChannels) {
    const test::FlightFixture f;
    const FlightLog log = FlightLog::fromJson(f.build().at("logs").at(0));
    const Timeline tl = TimelineBuilder({}).build(log);
    ASSERT_EQ(tl.size(), 601u);
    EXPECT_DOUBLE_EQ(tl.hz, 10.0);
    const std::size_t end = tl.size() - 1;
    EXPECT_NEAR(tl.at("usedMah", end), 10.0 * 60.0 / 3.6, 0.01);
    EXPECT_NEAR(tl.at("usedWh", end), 10.0 * (16.8 * 60.0 - 0.015 * 3600.0) / 3600.0, 0.01);
    EXPECT_NEAR(tl.at("cellV", 0), 4.2, 1e-9);
    EXPECT_NEAR(tl.at("distanceM", end), 480.0, 2.0);
    EXPECT_NEAR(tl.at("homeDistM", tl.indexAt(40.0)), 380.0, 2.0);
    EXPECT_NEAR(tl.at("altM", tl.indexAt(25.0)), 30.0, 1e-9) << "GPS altitude above home without a baro";
    EXPECT_NEAR(tl.at("climbMps", tl.indexAt(25.0)), 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(tl.at("armed", tl.indexAt(1.0)), 0.0);
    EXPECT_DOUBLE_EQ(tl.at("armed", tl.indexAt(10.0)), 1.0);
    EXPECT_DOUBLE_EQ(tl.at("rssiPct", tl.indexAt(31.0)), 20.0);
    EXPECT_DOUBLE_EQ(tl.at("sats", 5), 14.0);
    EXPECT_FALSE(hasValue(tl.at("nothing", 0)));
    EXPECT_EQ(tl.indexAt(std::nan("")), 0u);
    EXPECT_EQ(tl.indexAt(-5.0), 0u);
    EXPECT_EQ(tl.indexAt(1e300), end);
    const json j = tl.toJson();
    EXPECT_EQ(j.at("t").size(), 601u);
    EXPECT_TRUE(j.at("channels").contains("lat"));
}

TEST(TimelineBuilder, LeavesOutWhatTheLogDoesNotHave) {
    test::FlightFixture f;
    f.withCurrent = false;
    f.withGps = false;
    const Timeline tl = TimelineBuilder({}).build(FlightLog::fromJson(f.build().at("logs").at(0)));
    EXPECT_FALSE(tl.samples.has("usedMah"));
    EXPECT_FALSE(tl.samples.has("lat"));
    EXPECT_FALSE(tl.samples.has("altM"));
    EXPECT_TRUE(tl.samples.has("voltageV"));
}

TEST(TimelineBuilder, HeaderOnlyLogHasOneEmptySample) {
    const json entry = {{"index", 0}, {"status", "header-only"}, {"firmware", json::object()},
                        {"channels", json::object()}, {"notes", json::array()}};
    const Timeline tl = TimelineBuilder({}).build(FlightLog::fromJson(entry));
    EXPECT_EQ(tl.size(), 1u);
    EXPECT_EQ(tl.indexAt(99.0), 0u);
}
