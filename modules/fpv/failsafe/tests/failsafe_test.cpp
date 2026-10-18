#include "failsafe.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace fpv;
using namespace fpv::failsafe;

namespace {

class DistanceProbe final : public ILinkProbe {
public:
    DistanceProbe(const IWorld& w, LatLon home, double zeroAtM) : w_(w), home_(home), zero_(zeroAtM) {}
    double marginDb(const LatLon& p, double) const override { return (zero_ - w_.inverse(home_, p).distanceM) / 100.0; }

private:
    const IWorld& w_;
    LatLon home_;
    double zero_;
};

// A 60 m hill 1 km out, between the pilot and the far end of the route.
LocalWorld hillWorld() {
    return LocalWorld(
        [](const LatLon& p) {
            const double d = (p.lon - 35.2) * 73800.0;
            return std::optional<double>(d > 950.0 && d < 1050.0 ? 160.0 : 100.0);
        },
        10.0);
}

json mission(const IWorld& w, double km, double agl) {
    const LatLon a{48.5, 35.2};
    const LatLon b = w.direct(a, 90.0, km * 1000.0).point;
    return {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", agl}}}},
            {"returnToPilot", true}, {"evaluate", "control"}};
}

}  // namespace

TEST(Failsafe, NoLossNoAction) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 1e9);
    const json r = analyze({{"mission", mission(w, 3.0, 100.0)}}, w, probe, std::nullopt, std::nullopt);
    EXPECT_FALSE(r.at("data").at("controlLost").get<bool>());
}

TEST(Failsafe, RthAtCurrentAltitudeHitsTheHillAndSuggestsAltitude) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2500.0);
    // Flying at 30 m AGL over flat 100 m ground: 130 m MSL, the hill top is 160 m.
    const json r = analyze({{"mission", mission(w, 3.0, 30.0)}, {"options", {{"altitudeMode", "current"}, {"terrainClearanceM", 10.0}}}},
                           w, probe, std::nullopt, std::nullopt);
    const json& d = r.at("data");
    EXPECT_NEAR(d.at("loss").at("distanceAlongRouteM").get<double>(), 2500.0, 1.0);
    // AGL at the loss point is interpolated from 1.5 m to 30 m over 3 km.
    const double agl = 1.5 + (30.0 - 1.5) * d.at("loss").at("distanceAlongRouteM").get<double>() / 3000.0;
    EXPECT_NEAR(d.at("rth").at("cruiseMslM").get<double>(), 100.0 + agl, 1e-6);
    EXPECT_NEAR(d.at("rth").at("worstTerrainGapM").get<double>(), 100.0 + agl - 160.0, 1e-6);
    // Lowest altitude above home that clears the hill by 10 m: 160 + 10 - 100 = 70 m.
    EXPECT_EQ(d.at("rth").at("suggestedAltitudeAboveHomeM").get<double>(), 70.0);
    EXPECT_EQ(r.at("reasons").back().at("fix").at("patch").at("modules").at("fpv.failsafe").at("rthAltitudeAboveHomeM"), 70.0);
}

TEST(Failsafe, EnergyIncludesOutboundClimbAndReturn) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    const json drone = {{"id", "d"}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0}, {"cruisePowerW", 100.0},
                        {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}};
    const json battery = {{"id", "b"}, {"cellsSeries", 1}, {"capacityAh", 1.0}, {"nominalCellVoltage", 1.0}, {"usableFraction", 1.0}};
    const json r = analyze({{"mission", mission(w, 3.0, 100.0)}, {"options", {{"altitudeMode", "fixed"}, {"rthAltitudeAboveHomeM", 150.0}}}},
                           w, probe, drone, battery);
    const json& rth = r.at("data").at("rth");
    EXPECT_GT(rth.at("energyWh").get<double>(), rth.at("usableWh").get<double>());
    EXPECT_EQ(r.at("reasons").back().at("severity"), "blocked");
}

namespace {

json lossRequest(const IWorld& w, const json& options, const std::string& evaluate = "control") {
    json m = mission(w, 3.0, 100.0);
    m["evaluate"] = evaluate;
    return {{"mission", m}, {"options", options}};
}

const json kDrone = {{"id", "d"}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0}, {"cruisePowerW", 100.0},
                     {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}};
const json kBattery = {{"id", "b"}, {"cellsSeries", 6}, {"capacityAh", 2.0}, {"nominalCellVoltage", 3.7}, {"usableFraction", 0.8},
                       {"temperatureDerating", {{{"celsius", 0}, {"factor", 0.5}}, {{"celsius", 25}, {"factor", 1.0}}}}};

}  // namespace

TEST(Failsafe, NeedsTheControlLink) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    const json r = analyze(lossRequest(w, json::object(), "video"), w, probe, std::nullopt, std::nullopt);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
    EXPECT_FALSE(r.at("data").contains("controlLost"));
}

TEST(Failsafe, LandAndHoverStopAtTheLossPoint) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    for (const char* action : {"land", "hover"}) {
        const json r = analyze(lossRequest(w, {{"action", action}}), w, probe, std::nullopt, std::nullopt);
        EXPECT_TRUE(r.at("data").at("controlLost").get<bool>());
        EXPECT_FALSE(r.at("data").contains("rth")) << action;
        ASSERT_EQ(r.at("reasons").size(), 1u);
        EXPECT_EQ(r.at("reasons").at(0).at("severity"), "blocked");
        EXPECT_NE(r.at("reasons").at(0).at("text").get<std::string>().find(action == std::string("land") ? "lands" : "hovers"),
                  std::string::npos);
    }
}

TEST(Failsafe, MaxModeFliesTheHigherOfCurrentAndFixed) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    const json low = analyze(lossRequest(w, {{"rthAltitudeAboveHomeM", 20.0}}), w, probe, std::nullopt, std::nullopt);
    const json high = analyze(lossRequest(w, {{"rthAltitudeAboveHomeM", 200.0}}), w, probe, std::nullopt, std::nullopt);
    const double lossAgl = low.at("data").at("loss").at("aglM").get<double>();
    EXPECT_NEAR(low.at("data").at("rth").at("altitudeAboveHomeM").get<double>(), lossAgl, 1e-6);
    EXPECT_NEAR(high.at("data").at("rth").at("altitudeAboveHomeM").get<double>(), 200.0, 1e-6);
}

TEST(Failsafe, UsableEnergyFollowsTemperatureAndTheBatteryModule) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    json req = lossRequest(w, {{"temperatureC", 0.0}});
    const json cold = analyze(req, w, probe, kDrone, kBattery);
    EXPECT_NEAR(cold.at("data").at("rth").at("usableWh").get<double>(), 6 * 3.7 * 2.0 * 0.5 * 0.8, 1e-9);

    req["options"] = json::object();
    req["weather"] = {{"temperatureC", 25.0}, {"airDensityKgM3", 1.2}, {"windSpeedMps", 0.0}, {"windFromDeg", 0.0}};
    const json warm = analyze(req, w, probe, kDrone, kBattery);
    EXPECT_NEAR(warm.at("data").at("rth").at("usableWh").get<double>(), 6 * 3.7 * 2.0 * 0.8, 1e-9);

    req["modules"] = {{"fpv.battery", {{"usableWh", 3.0}}}};
    const json quoted = analyze(req, w, probe, kDrone, kBattery);
    EXPECT_DOUBLE_EQ(quoted.at("data").at("rth").at("usableWh").get<double>(), 3.0);
    EXPECT_EQ(quoted.at("reasons").back().at("severity"), "blocked");
}

TEST(Failsafe, HeadwindHomeIsUnflyable) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    // 12 m/s from the west against a 10 m/s airspeed: the drone cannot fly home westwards.
    json req = lossRequest(w, json::object());
    req["weather"] = {{"windSpeedMps", 12.0}, {"windFromDeg", 270.0}};
    const json r = analyze(req, w, probe, kDrone, kBattery);
    EXPECT_EQ(r.at("reasons").back().at("severity"), "blocked");
    EXPECT_NE(r.at("reasons").back().at("text").get<std::string>().find("not flyable"), std::string::npos);
}

TEST(Failsafe, TemperatureOutsideTheMeasuredRangeSkipsTheEnergyCheck) {
    const auto w = hillWorld();
    const DistanceProbe probe(w, {48.5, 35.2}, 2000.0);
    const json r = analyze(lossRequest(w, {{"temperatureC", 40.0}}), w, probe, kDrone, kBattery);
    EXPECT_EQ(r.at("reasons").back().at("severity"), "warn");
    EXPECT_NE(r.at("reasons").back().at("text").get<std::string>().find("energy not checked"), std::string::npos);
}
