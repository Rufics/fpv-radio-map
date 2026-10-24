#include "envelope.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>

using namespace fpv;
using namespace fpv::airframe;

namespace {

constexpr double kDeg = rfcore::constants::kPi / 180.0;

json wing(double airspeed = 20.0, double stall = 11.0, double bank = 25.0, double climb = 10.0) {
    return {{"id", "wing"}, {"airframe", "fixed-wing"}, {"cruiseAirspeedMps", airspeed},
            {"fixedWing", {{"stallSpeedMps", stall}, {"maxBankDeg", bank}, {"maxClimbAngleDeg", climb}, {"maxDiveAngleDeg", 8.0}}}};
}

LocalWorld flat() {
    return LocalWorld([](const LatLon&) { return std::optional<double>(100.0); }, 10.0);
}

// An 80 m ridge from 300 to 400 m east of the pilot.
LocalWorld ridge() {
    return LocalWorld(
        [](const LatLon& p) {
            const double d = (p.lon - 35.2) * 73800.0;
            return std::optional<double>(d > 300.0 && d < 400.0 ? 180.0 : 100.0);
        },
        10.0);
}

/// Pilot at (48.5, 35.2), then each leg as (azimuth, metres, height).
struct LegSpec {
    double azimuthDeg;
    double lengthM;
    double heightM;
};

json route(const IWorld& w, const std::vector<LegSpec>& legs, bool returnToPilot = false, const std::string& ref = "agl") {
    LatLon at{48.5, 35.2};
    json wps = json::array();
    const std::string field = ref == "launch" ? "altM" : "aglM";
    for (const LegSpec& l : legs) {
        at = w.direct(at, l.azimuthDeg, l.lengthM).point;
        wps.push_back({{"lat", at.lat}, {"lon", at.lon}, {field, l.heightM}});
    }
    return {{"pilot", {{"lat", 48.5}, {"lon", 35.2}, {"aglM", 1.5}}}, {"waypoints", wps},
            {"returnToPilot", returnToPilot}, {"altitudeRef", ref}};
}

bool hasReason(const json& r, const std::string& severity, const std::string& text) {
    for (const json& x : r.at("reasons")) {
        if (x.at("severity") == severity && x.at("text").get<std::string>().find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(Airframe, MultirotorByDefault) {
    const Airframe a = Airframe::fromDrone({{"id", "quad"}});
    EXPECT_FALSE(a.fixedWing());
    EXPECT_EQ(a.name(), "multirotor");
    EXPECT_THROW((void)a.envelope(), NotFixedWing);
    EXPECT_THROW((void)Airframe::fromDrone({{"id", "w"}, {"airframe", "fixed-wing"}}), rfmod::BadRequest);
}

TEST(Airframe, TurnRadiusAndStallInATurn) {
    const Airframe a = Airframe::fromDrone(wing(20.0, 11.0, 30.0));
    EXPECT_NEAR(a.turnRadiusM(20.0), 400.0 / (9.80665 * std::tan(30.0 * kDeg)), 1e-9);
    EXPECT_NEAR(a.turningStallSpeedMps(), 11.0 / std::sqrt(std::cos(30.0 * kDeg)), 1e-12);
    EXPECT_NEAR(a.loiterRadiusM(20.0), a.turnRadiusM(20.0), 1e-12);
    json withLoiter = wing();
    withLoiter["fixedWing"]["loiterRadiusM"] = 75.0;
    EXPECT_DOUBLE_EQ(Airframe::fromDrone(withLoiter).loiterRadiusM(20.0), 75.0);
}

TEST(Envelope, MultirotorHasNothingToCheck) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 400.0, 120.0}})}}, w, {{"id", "quad"}, {"cruiseAirspeedMps", 10.0}});
    EXPECT_EQ(r.at("data").at("airframe"), "multirotor");
    EXPECT_TRUE(hasReason(r, "info", "Multirotor"));
}

TEST(Envelope, ClimbLimitedByTheEnvelopeAngle) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 400.0, 120.0}, {0.0, 2000.0, 120.0}})}}, w, wing());
    const json& v = r.at("data").at("vertical");
    ASSERT_EQ(v.at("arrivals").size(), 1u);
    const double below = 118.5 - 400.0 * std::tan(10.0 * kDeg);
    EXPECT_NEAR(v.at("arrivals").at(0).at("belowPlanM").get<double>(), below, 1e-6);
    EXPECT_NEAR(v.at("arrivals").at(0).at("extraClimbM").get<double>(), below / std::tan(10.0 * kDeg), 1e-6);
    EXPECT_NEAR(v.at("worst").at("belowPlanM").get<double>(), below, 1e-6);
    EXPECT_FALSE(v.contains("terrain"));  // flat ground: low, but clear of it
    EXPECT_TRUE(hasReason(r, "warn", "below the planned height"));
}

TEST(Envelope, GentleClimbFollowsThePlan) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 2000.0, 120.0}})}}, w, wing());
    EXPECT_TRUE(r.at("data").at("vertical").at("arrivals").empty());
    EXPECT_FALSE(r.at("data").at("vertical").contains("worst"));
    EXPECT_TRUE(hasReason(r, "info", "can follow the planned heights"));
}

TEST(Envelope, SteepClimbOverARidgeHitsIt) {
    // 60 m above the ridge is planned, but at 10° the plane is only ~55 m up at 300 m.
    const auto w = ridge();
    const json r = analyze({{"mission", route(w, {{90.0, 1000.0, 60.0}})}}, w, wing());
    const json& v = r.at("data").at("vertical");
    ASSERT_TRUE(v.contains("terrain")) << v;
    EXPECT_LT(v.at("terrain").at("gapM").get<double>(), 0.0);
    EXPECT_NEAR(v.at("terrain").at("distanceAlongRouteM").get<double>(), 300.0, 10.0);
    EXPECT_TRUE(hasReason(r, "blocked", "flies into terrain"));
}

TEST(Envelope, LaunchReferencedHeightsAreAStraightLine) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 400.0, 120.0}}, false, "launch")}}, w, wing());
    const double below = 118.5 - 400.0 * std::tan(10.0 * kDeg);
    EXPECT_NEAR(r.at("data").at("vertical").at("arrivals").at(0).at("belowPlanM").get<double>(), below, 1e-6);
}

TEST(Envelope, TurnsAtTheirRadius) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 2000.0, 100.0}, {0.0, 2000.0, 100.0}})}}, w, wing());
    const json& t = r.at("data").at("turns");
    ASSERT_EQ(t.size(), 1u);
    const double radius = 400.0 / (9.80665 * std::tan(25.0 * kDeg));
    EXPECT_NEAR(t.at(0).at("headingChangeDeg").get<double>(), 90.0, 0.1);
    EXPECT_NEAR(t.at(0).at("radiusM").get<double>(), radius, 1e-9);
    EXPECT_NEAR(t.at(0).at("leadM").get<double>(), radius, 0.5);  // tan 45° = 1
    EXPECT_NEAR(t.at(0).at("insideM").get<double>(), radius * (std::sqrt(2.0) - 1.0), 0.5);
    EXPECT_TRUE(t.at(0).at("fits").get<bool>());
    EXPECT_TRUE(hasReason(r, "info", "every turn fits"));
}

TEST(Envelope, TightTurnOnAShortLegIsAWarning) {
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 2000.0, 100.0}, {0.0, 120.0, 100.0}, {270.0, 2000.0, 100.0}})}}, w, wing());
    const json& t = r.at("data").at("turns");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_FALSE(t.at(0).at("fits").get<bool>());
    EXPECT_TRUE(hasReason(r, "warn", "overshoots"));
}

TEST(Envelope, OutAndBackIsAUTurnAndWindWidensIt) {
    const auto w = flat();
    const json m = route(w, {{90.0, 2000.0, 100.0}}, true);
    const json calm = analyze({{"mission", m}}, w, wing());
    const json& t = calm.at("data").at("turns");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_TRUE(t.at(0).at("uTurn").get<bool>());
    EXPECT_TRUE(hasReason(calm, "info", "turn-around"));
    const json windy = analyze({{"mission", m}, {"options", {{"windSpeedMps", 5.0}}}}, w, wing());
    EXPECT_NEAR(windy.at("data").at("turns").at(0).at("radiusM").get<double>(),
                625.0 / (9.80665 * std::tan(25.0 * kDeg)), 1e-9);
}

TEST(Envelope, StallMarginInTurnsAndHeadwind) {
    const auto w = flat();
    const json m = route(w, {{90.0, 2000.0, 100.0}});
    EXPECT_TRUE(hasReason(analyze({{"mission", m}}, w, wing(11.5, 11.0, 30.0)), "blocked", "stalls"));
    EXPECT_TRUE(hasReason(analyze({{"mission", m}}, w, wing(13.0, 11.0, 30.0)), "warn", "gust"));
    EXPECT_TRUE(hasReason(analyze({{"mission", m}, {"weather", {{"windSpeedMps", 21.0}}}}, w, wing()), "blocked", "headway"));
}

TEST(Envelope, MissingTerrainIsAWarning) {
    const LocalWorld hole([](const LatLon& p) { return p.lon > 35.21 ? std::nullopt : std::optional<double>(100.0); }, 10.0);
    const json r = analyze({{"mission", route(hole, {{90.0, 2000.0, 100.0}})}}, hole, wing());
    EXPECT_TRUE(hasReason(r, "warn", "not checked"));
}

TEST(Envelope, SteepDescentArrivesHighButTheLandingCirclesDown) {
    // 120 m down to 20 m over 300 m needs 18.4°, beyond the 8° dive limit; the way home from
    // 20 m to the pilot is a landing and is not reported.
    const auto w = flat();
    const json r = analyze({{"mission", route(w, {{90.0, 2000.0, 120.0}, {0.0, 300.0, 20.0}}, true)}}, w, wing());
    const json& high = r.at("data").at("vertical").at("highArrivals");
    ASSERT_EQ(high.size(), 1u) << high;
    EXPECT_EQ(high.at(0).at("leg"), 1);
    EXPECT_NEAR(high.at(0).at("abovePlanM").get<double>(), 100.0 - 300.0 * std::tan(8.0 * kDeg), 1e-6);
    EXPECT_TRUE(hasReason(r, "warn", "above the planned height"));
    EXPECT_FALSE(hasReason(r, "info", "can follow the planned heights"));
}
