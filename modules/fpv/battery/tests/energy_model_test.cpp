#include "analysis.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace fpv;
using namespace fpv::battery;

namespace {

const json kDrone = {{"id", "d"}, {"version", 3}, {"massKg", 2.0}, {"cruiseAirspeedMps", 15.0}, {"cruisePowerW", 300.0},
                     {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}};
const json kBattery = {{"id", "b"}, {"version", 2}, {"cellsSeries", 6}, {"capacityAh", 1.3}, {"nominalCellVoltage", 3.7},
                       {"usableFraction", 0.8},
                       {"temperatureDerating", {{{"celsius", -10}, {"factor", 0.7}}, {{"celsius", 0}, {"factor", 0.85}}, {{"celsius", 25}, {"factor", 1.0}}}}};

LocalWorld flat(double h = 100.0) {
    return LocalWorld([h](const LatLon&) { return std::optional<double>(h); }, 30.0);
}

json missionTo(const IWorld& w, double km, bool back = true) {
    const LatLon a{48.5, 35.2};
    const LatLon b = w.direct(a, 45.0, km * 1000.0).point;
    return {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", 1.5}, {"label", "Target"}}}},
            {"returnToPilot", back}};
}

}  // namespace

TEST(Battery, DeratingIsMeasuredOrInterpolatedNeverExtrapolated) {
    const BatterySpec b = BatterySpec::fromJson(kBattery);
    EXPECT_DOUBLE_EQ(b.nominalWh(), 6 * 3.7 * 1.3);
    EXPECT_DOUBLE_EQ(b.deratingAt(0.0).factor, 0.85);
    EXPECT_DOUBLE_EQ(b.deratingAt(-5.0).factor, 0.775);
    EXPECT_NE(b.deratingAt(-5.0).note.find("interpolated"), std::string::npos);
    EXPECT_THROW((void)b.deratingAt(30.0), std::domain_error);
    EXPECT_DOUBLE_EQ(b.deratingAt(std::nullopt).factor, 1.0);
}

TEST(Battery, WindTriangle) {
    const auto w = flat();
    Air air;
    air.windSpeedMps = 9.0;
    air.windFromDeg = 0.0;  // from the north
    const EnergyModel m(DroneSpec::fromJson(kDrone), air, w);
    EXPECT_DOUBLE_EQ(m.groundSpeedMps(180.0), 24.0);  // tailwind
    EXPECT_DOUBLE_EQ(m.groundSpeedMps(0.0), 6.0);     // headwind
    EXPECT_NEAR(m.groundSpeedMps(90.0), 12.0, 1e-12);  // sqrt(225 - 81)
    air.windSpeedMps = 15.0;
    const EnergyModel strong(DroneSpec::fromJson(kDrone), air, w);
    EXPECT_THROW((void)strong.groundSpeedMps(0.0), Unflyable);
    EXPECT_THROW((void)strong.groundSpeedMps(90.0), Unflyable);
}

TEST(Battery, DensityScalingOnlyWhenMeasured) {
    const auto w = flat();
    json d = kDrone;
    d["powerDensityExponent"] = 1.5;
    Air air;
    EXPECT_THROW(EnergyModel(DroneSpec::fromJson(d), air, w), std::domain_error);
    air.densityKgM3 = 1.0;
    const EnergyModel m(DroneSpec::fromJson(d), air, w);
    EXPECT_DOUBLE_EQ(m.cruisePowerW(), 300.0 * std::pow(1.225 / 1.0, 1.5));
}

TEST(Battery, ClimbOverTerrainCountsOnlyRises) {
    // A ridge: up 50 m and down again. Potential energy m g Δh / η for the rise only.
    const LatLon a{48.5, 35.2};
    const auto base = flat();
    const LatLon mid = base.direct(a, 90.0, 1000.0).point;
    const LocalWorld ridge(
        [&base, &a](const LatLon& p) {
            const double d = base.inverse(a, p).distanceM;
            // Flat 40 m wide top, so any sampling step up to 10 m lands on it.
            return std::optional<double>(100.0 + 50.0 * std::clamp(1.0 - (std::abs(d - 1000.0) - 20.0) / 500.0, 0.0, 1.0));
        },
        10.0);
    const LatLon b = base.direct(a, 90.0, 2000.0).point;
    (void)mid;
    const EnergyModel m(DroneSpec::fromJson(kDrone), Air{}, ridge);
    const double cruise = 300.0 * (2000.0 / 15.0) / 3600.0;
    const double climb = 2.0 * 9.80665 * 50.0 / 0.5 / 3600.0;
    EXPECT_NEAR(m.segmentWh(a, 10.0, b, 10.0, AltitudeModel(AltitudeReference::Agl, 0.0)), cruise + climb, 1e-6);
    // Above the launch point the drone holds its altitude over the ridge: only the ends count.
    const AltitudeModel launch(AltitudeReference::Launch, 100.0);
    EXPECT_NEAR(m.segmentWh(a, 60.0, b, 60.0, launch), cruise, 1e-6);
    EXPECT_NEAR(m.segmentWh(a, 60.0, b, 90.0, launch), cruise + 2.0 * 9.80665 * 30.0 / 0.5 / 3600.0, 1e-6);
    EXPECT_NEAR(m.segmentWh(a, 90.0, b, 60.0, launch), cruise, 1e-6);
}

TEST(Battery, EnoughEnergyGivesNoTurnBack) {
    const auto w = flat();
    const json r = analyze({{"mission", missionTo(w, 1.0)}, {"options", {{"temperatureC", 25.0}}}}, w, kDrone, kBattery);
    const json& d = r.at("data");
    EXPECT_NEAR(d.at("neededWh").get<double>(), 300.0 * (2000.0 / 15.0) / 3600.0, 1e-6);
    EXPECT_FALSE(d.contains("turnBack"));
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "warn");  // no wind forecast
}

TEST(Battery, TurnBackIsTheLastPointYouCanReturnFrom) {
    // Usable: 6*3.7*1.3*0.8 = 23.088 Wh → 277.056 s at 300 W → 4155.84 m of calm flight.
    // Out and back on flat ground: turn back at half that, 2077.92 m.
    const auto w = flat();
    const json r = analyze({{"mission", missionTo(w, 5.0)}, {"options", {{"temperatureC", 25.0}}}}, w, kDrone, kBattery);
    const json& d = r.at("data");
    const double usable = 6 * 3.7 * 1.3 * 0.8;
    const double reach = usable * 3600.0 / 300.0 * 15.0;
    EXPECT_NEAR(d.at("usableWh").get<double>(), usable, 1e-12);
    EXPECT_NEAR(d.at("rangeM").get<double>(), reach, 2e-3);
    EXPECT_NEAR(d.at("turnBack").at("distanceAlongRouteM").get<double>(), reach / 2.0, 2e-3);
    EXPECT_NEAR(d.at("turnBack").at("returnM").get<double>(), reach / 2.0, 2e-3);
    const json& reasonJ = r.at("reasons").back();
    EXPECT_EQ(reasonJ.at("severity"), "blocked");
    const json& patch = reasonJ.at("fix").at("patch");
    ASSERT_EQ(patch.at("waypoints").size(), 1u);
    EXPECT_EQ(patch.at("waypoints").at(0).at("label"), "T");
}

TEST(Battery, TemperatureOutsideMeasuredRangeIsReportedNotGuessed) {
    const auto w = flat();
    const json r = analyze({{"mission", missionTo(w, 1.0)}, {"options", {{"temperatureC", 40.0}}}}, w, kDrone, kBattery);
    EXPECT_NE(r.at("reasons").back().at("text").get<std::string>().find("not extrapolated"), std::string::npos);
}
