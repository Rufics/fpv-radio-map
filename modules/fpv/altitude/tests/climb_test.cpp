// The climb assistant: the energy-optimal profile, the simpler strategies, and their fixes.

#include "climb_assistant.hpp"
#include "climb_planner.hpp"
#include "profile_fit.hpp"

#include <fake_host.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>

using namespace fpv;
using namespace fpv::altitude;
using namespace fpv::test;

namespace {

const LatLon kPilot{48.5, 35.2};
constexpr double kInf = std::numeric_limits<double>::infinity();

/// A corridor over ground at 100 m: the floor at `floorAgl(s)` above it, the ceiling 120 m above
/// it, the plan at `plannedAgl`, and the take-off zone the first and last `takeoffM`.
std::vector<CorridorPoint> corridor(const std::function<double(double)>& floorAgl, double lengthM, double plannedAgl,
                                    bool returns, double takeoffM = 200.0) {
    std::vector<CorridorPoint> out;
    const auto samples = static_cast<int>(std::floor(lengthM / 100.0 + 1e-9));
    for (int k = 0; k <= samples; ++k) {
        const double s = 100.0 * k;
        CorridorPoint c{};
        c.s = s;
        c.p = RoutePoint{kPilot, plannedAgl, 0, 90.0, plannedAgl};
        c.groundMslM = 100.0;
        const bool home = s == 0.0 || (returns && s >= lengthM - 1e-9);
        c.plannedMslM = 100.0 + (home ? 1.5 : plannedAgl);
        c.nearPilot = s < takeoffM || (returns && lengthM - s < takeoffM);
        c.terrainFloorMslM = 100.0 + (home ? 0.0 : floorAgl(s));
        c.ceilingMslM = 220.0;
        out.push_back(c);
    }
    return out;
}

/// Segments at 10 m/s with the drone climbing 2 m/s and descending 4 m/s: 20 m up and 40 m down
/// per 100 m stretch, free in the take-off zone.
std::vector<Segment> segmentsFor(const std::vector<CorridorPoint>& pts) {
    std::vector<Segment> out;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const bool free = pts[i].nearPilot || pts[i + 1].nearPilot;
        out.push_back({free ? kInf : 20.0, free ? kInf : 40.0, 10.0, 2.0, 4.0});
    }
    return out;
}

void expectOverFloor(const std::vector<CorridorPoint>& pts, const std::vector<double>& msl) {
    ASSERT_EQ(msl.size(), pts.size());
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) EXPECT_GE(msl[i], pts[i].floorMslM() - 1e-6) << "at s = " << pts[i].s;
}

LocalWorld flat() {
    return LocalWorld([](const LatLon&) { return std::optional<double>(100.0); }, 10.0);
}

LocalWorld ridgeWorld() {
    // An 80 m ridge from 2.8 to 3.2 km east of the pilot: its floor, 210 m, is under the 220 m
    // ceiling before it, so a climb within the rate gets over it.
    return LocalWorld([](const LatLon& p) {
        const double x = (p.lon - kPilot.lon) * 73800.0;
        return std::optional<double>(x > 2800.0 && x < 3200.0 ? 180.0 : 100.0);
    }, 10.0);
}

json oneLeg(const IWorld& world, double km, double heightM, const std::string& ref, bool returns) {
    const LatLon p = world.direct(kPilot, 90.0, km * 1000.0).point;
    return {{"altitudeRef", ref},
            {"pilot", {{"lat", kPilot.lat}, {"lon", kPilot.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", p.lat}, {"lon", p.lon}, {"label", "W1"}, {ref == "launch" ? "altM" : "aglM", heightM}}}},
            {"returnToPilot", returns}};
}

const json* strategy(const json& climb, const std::string& id) {
    for (const json& s : climb.at("strategies")) {
        if (s.at("id") == id) return &s;
    }
    return nullptr;
}

}  // namespace

TEST(ClimbPlanner, FlatGroundClimbsOnceInTheTakeOffZone) {
    const auto pts = corridor([](double) { return 30.0; }, 3000.0, 60.0, false);
    const ClimbPlanner planner(pts, segmentsFor(pts), false, 5.0);
    const ClimbProfile p = planner.energyOptimal();
    EXPECT_TRUE(p.withinClimbRate);
    expectOverFloor(pts, p.mslM);
    // To 130 m (the floor, on the grid) and level after: 28.5 m of climb from the pilot's 101.5 m.
    EXPECT_NEAR(planner.stats(p.mslM).riseM, 28.5, 1e-6);
    EXPECT_NEAR(p.mslM.back(), 130.0, 1e-6);
    // 300 s at 10 m/s, but the first 100 m take as long as climbing 28.5 m at 2 m/s.
    EXPECT_NEAR(planner.stats(p.mslM).timeS, 290.0 + 28.5 / 2.0, 1e-6);
}

TEST(ClimbPlanner, AClimbToARidgeStartsEarlyEnoughAndKeepsToTheRate) {
    // The floor rises 100 m at 2 km; the drone climbs 20 m per 100 m.
    const auto pts = corridor([](double s) { return s >= 2000.0 && s <= 2400.0 ? 130.0 : 30.0; }, 4000.0, 60.0, false);
    const ClimbPlanner planner(pts, segmentsFor(pts), false, 5.0);
    const ClimbProfile p = planner.energyOptimal();
    EXPECT_TRUE(p.withinClimbRate);
    expectOverFloor(pts, p.mslM);
    const ProfileStats st = planner.stats(p.mslM);
    EXPECT_LE(st.steepestClimbMps, 2.0 + 1e-6);
    // Climbing as late as it can: still at the low floor 600 m before the ridge, 20 m up per 100 m after.
    EXPECT_NEAR(p.mslM[14], 130.0, 1e-6);
    EXPECT_NEAR(p.mslM[20], 230.0, 1e-6);
    // Down again after the ridge: descending is free, and lower is preferred.
    EXPECT_NEAR(p.mslM.back(), 130.0, 1e-6);
}

TEST(ClimbPlanner, RatesCoverDescentsWithTheGivenSlack) {
    const auto pts = corridor([](double) { return 30.0; }, 3000.0, 60.0, false);
    const ClimbPlanner planner(pts, segmentsFor(pts), false, 5.0);
    // 100 m stretches at 10 m/s: 4 m/s down is 40 m a stretch, and 0.05 m/s of slack is 0.5 m more.
    const auto dropAt = [&](double dropM) {
        std::vector<double> h(pts.size(), 200.0);
        for (std::size_t i = 16; i < h.size(); ++i) h[i] = 200.0 - dropM;
        return h;
    };
    EXPECT_TRUE(planner.withinRates(dropAt(40.0)));
    EXPECT_FALSE(planner.withinRates(dropAt(40.3)));
    EXPECT_TRUE(planner.withinRates(dropAt(40.3), 0.05));
    EXPECT_FALSE(planner.withinRates(dropAt(41.0), 0.05));
}

TEST(ClimbPlanner, NoProfileWithinTheRateIsMarked) {
    // The ceiling is 220 m and a wall needs 330 m one stretch later: no rate-limited way over it.
    auto pts = corridor([](double s) { return s >= 2000.0 && s <= 2200.0 ? 230.0 : 30.0; }, 3000.0, 60.0, false);
    for (CorridorPoint& c : pts) c.ceilingMslM = std::max(c.ceilingMslM, c.terrainFloorMslM);
    const ClimbPlanner planner(pts, segmentsFor(pts), false, 5.0);
    const ClimbProfile p = planner.energyOptimal();
    EXPECT_FALSE(p.withinClimbRate);
    expectOverFloor(pts, p.mslM);
}

TEST(ClimbPlanner, EnergyOptimalNeverClimbsMoreThanTheOtherStrategies) {
    const auto pts = corridor([](double s) { return 30.0 + 60.0 * std::sin(s / 700.0) * std::sin(s / 700.0); }, 6000.0, 80.0, true);
    const ClimbPlanner planner(pts, segmentsFor(pts), true, 5.0);
    const ClimbProfile energy = planner.energyOptimal();
    const ClimbProfile over = planner.overLaunch();
    const ClimbProfile gradual = planner.gradual();
    for (const ClimbProfile* p : {&energy, &over, &gradual}) {
        expectOverFloor(pts, p->mslM);
        EXPECT_NEAR(p->mslM.front(), 101.5, 1e-9) << p->id;
        EXPECT_NEAR(p->mslM.back(), 101.5, 1e-9) << p->id << ": a return ends at the pilot";
    }
    const double e = planner.stats(energy.mslM).riseM;
    EXPECT_LE(e, planner.stats(over.mslM).riseM + 1e-6);
    EXPECT_LE(e, planner.stats(gradual.mslM).riseM + 1e-6);
}

TEST(ClimbPlanner, GradualClimbsInAStraightLineToTheHighestFloor) {
    const auto pts = corridor([](double s) { return s >= 3000.0 && s <= 3100.0 ? 90.0 : 30.0; }, 4000.0, 60.0, false);
    const ClimbPlanner planner(pts, segmentsFor(pts), false, 5.0);
    const ClimbProfile p = planner.gradual();
    expectOverFloor(pts, p.mslM);
    // Up to the 130 m floor in the take-off zone, steadily from there to 190 m at 3 km, then level.
    EXPECT_NEAR(p.mslM[1], 130.0, 1e-6);
    EXPECT_NEAR(p.mslM[16], 160.0, 1e-6);
    EXPECT_NEAR(p.mslM[30], 190.0, 1e-6);
    EXPECT_NEAR(p.mslM.back(), 190.0, 1e-6);
}

TEST(ProfileFitter, AddsWaypointsOnlyWhereTheLineStraysFromTheProfile) {
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 60.0, "launch", false);
    const Route route(Mission::fromJson(mission), w);
    auto pts = corridor([](double) { return 30.0; }, route.length(), 60.0, false);
    std::vector<double> profile(pts.size(), 130.0);
    profile.front() = 101.5;
    const ProfileFitter fitter(pts, route, mission.at("waypoints"), 5.0);
    const FittedRoute fit = fitter.fit(profile);
    // The climb-out is the only bend: one added point near the pilot, then W1 at 30 m above launch.
    EXPECT_EQ(fit.added, 1U);
    ASSERT_EQ(fit.waypoints.size(), 2U);
    EXPECT_EQ(fit.waypoints[0].at("label"), "C1");
    EXPECT_EQ(fit.waypoints[0].at("heightSource"), "manual");
    EXPECT_NEAR(fit.waypoints[1].at("altM").get<double>(), 30.0, 0.1);
    for (std::size_t i = 0; i < pts.size(); ++i) EXPECT_LE(std::abs(fit.mslM[i] - profile[i]), 5.0 + 0.1) << "at s = " << pts[i].s;
}

TEST(ProfileFitter, AddsAWaypointWhereTheLineWouldCrossTheCeiling) {
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 100.0, "launch", false);
    const Route route(Mission::fromJson(mission), w);
    auto pts = corridor([](double) { return 0.0; }, route.length(), 100.0, false);
    // Well within the tolerance of a straight line, but the ceiling dips under it half way.
    const std::size_t mid = pts.size() / 2;
    std::vector<double> profile(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) profile[i] = 101.5 + (200.0 - 101.5) * pts[i].s / pts.back().s;
    profile[mid] -= 6.0;
    pts[mid].ceilingMslM = profile[mid] + 1.0;
    const FittedRoute fit = ProfileFitter(pts, route, mission.at("waypoints"), 50.0).fit(profile);
    EXPECT_EQ(fit.added, 1U);
    EXPECT_LE(fit.mslM[mid], pts[mid].ceilingMslM + 0.1);
}

TEST(ClimbAssist, AWastefulPlanGetsTheEnergyOptimalFix) {
    // 100 m above flat ground where 30 m clears it: every strategy, and advice with a fix.
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 100.0, "agl", true);
    const json r = analyze({{"mission", mission}, {"options", {{"stepM", 100.0}, {"linkFloor", false}}}}, w, {});
    const json& climb = r.at("data").at("climb");
    EXPECT_EQ(climb.at("recommended"), "energy");
    EXPECT_FALSE(climb.at("energyEstimated").get<bool>());
    EXPECT_NE(climb.at("notes").back().get<std::string>().find("follow the terrain between waypoints"), std::string::npos);
    ASSERT_EQ(climb.at("strategies").size(), 3U);
    const json* energy = strategy(climb, "energy");
    ASSERT_NE(energy, nullptr);
    EXPECT_LT(energy->at("riseM").get<double>(), climb.at("plan").at("riseM").get<double>());
    EXPECT_GE(energy->at("minClearanceM").get<double>(), -0.05);
    const json& fix = energy->at("fix");
    EXPECT_EQ(fix.at("id"), "fpv.altitude/climb-energy");
    EXPECT_FALSE(fix.at("preview").empty());
    const json& wps = fix.at("patch").at("waypoints");
    bool w1 = false;
    for (const json& x : wps) w1 = w1 || (x.at("label") == "W1" && x.at("heightSource") == "manual");
    EXPECT_TRUE(w1) << wps.dump();
    bool advised = false;
    for (const json& x : r.at("reasons")) advised = advised || (x.at("severity") == "info" && x.contains("fix"));
    EXPECT_TRUE(advised) << r.at("reasons").dump();
}

TEST(ClimbAssist, APlanUnderTheFloorIsAdvisedIntoTheCorridor) {
    // 10 m above flat ground where 30 m clears it: the energy-optimal profile climbs more, but stays in.
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 10.0, "agl", true);
    const json r = analyze({{"mission", mission}, {"options", {{"stepM", 100.0}, {"linkFloor", false}}}}, w, {});
    std::string advice;
    for (const json& x : r.at("reasons")) {
        if (x.at("severity") == "info" && x.contains("fix")) advice = x.at("text");
    }
    EXPECT_TRUE(advice.starts_with("Climb assistant: the energy-optimal profile keeps the whole route inside the corridor, climbing "))
        << advice;
}

TEST(ClimbAssist, APlanClimbingFasterThanTheDroneIsWarnedPerLeg) {
    // W1 at 30 m and W2 at 150 m only 300 m on: 120 m in 300 m at 15 m/s is 6 m/s, over 3 m/s.
    const LocalWorld w = flat();
    json mission = oneLeg(w, 2.0, 30.0, "agl", false);
    const LatLon p = w.direct(kPilot, 90.0, 2300.0).point;
    mission["waypoints"].push_back({{"lat", p.lat}, {"lon", p.lon}, {"label", "W2"}, {"aglM", 150.0}});
    const json r = analyze({{"mission", mission}, {"options", {{"stepM", 100.0}, {"linkFloor", false}, {"ceilingAglM", 200.0}}}}, w, {});
    bool warned = false;
    for (const json& x : r.at("reasons")) {
        const std::string text = x.at("text");
        if (text.find("the drone climbs 3.0 m/s") != std::string::npos) {
            warned = true;
            EXPECT_EQ(x.at("leg"), 1);
            EXPECT_TRUE(x.contains("fix"));
        }
    }
    EXPECT_TRUE(warned) << r.at("reasons").dump();
}

TEST(ClimbAssist, ARidgeIsClimbedWithinTheRateAndTheFixClearsIt) {
    const LocalWorld w = ridgeWorld();
    const json mission = oneLeg(w, 5.0, 40.0, "launch", true);
    const json r = analyze({{"mission", mission}, {"options", {{"stepM", 100.0}, {"linkFloor", false}, {"climbRateMps", 2.0}, {"speedMps", 10.0}}}}, w, {});
    const json& climb = r.at("data").at("climb");
    const json* energy = strategy(climb, "energy");
    ASSERT_NE(energy, nullptr);
    EXPECT_TRUE(energy->at("withinClimbRate").get<bool>()) << energy->dump();
    EXPECT_GE(energy->at("minClearanceM").get<double>(), -0.05);
    EXPECT_LE(energy->at("steepestClimbMps").get<double>(), 2.05);
    EXPECT_GT(energy->at("addedWaypoints").get<int>(), 0);
    // The plan at 40 m above launch goes into the ridge.
    EXPECT_LT(climb.at("plan").at("minClearanceM").get<double>(), 0.0);
}

TEST(ClimbAssist, EnergyForTheMissionsDrone) {
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 100.0, "agl", true);
    const json drone = {{"id", "d"}, {"version", 2}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0}, {"cruisePowerW", 100.0},
                        {"climb", {{"method", "measured"}, {"climbPowerW", 150.0}, {"climbRateMps", 2.5}}}};
    const json r = analyze({{"mission", mission}, {"options", {{"linkFloor", false}}}}, w, {}, std::optional<json>(std::in_place, drone));
    const json& climb = r.at("data").at("climb");
    EXPECT_TRUE(climb.at("energyEstimated").get<bool>());
    EXPECT_DOUBLE_EQ(climb.at("climbRateMps").get<double>(), 2.5);
    EXPECT_EQ(climb.at("drone").at("id"), "d");
    const json* energy = strategy(climb, "energy");
    ASSERT_NE(energy, nullptr);
    EXPECT_LT(energy->at("energyWh").get<double>(), climb.at("plan").at("energyWh").get<double>());
}

TEST(ClimbAssist, TheSpeedOptionFliesTheDroneAtThatAirspeed) {
    const LocalWorld w = flat();
    const json mission = oneLeg(w, 3.0, 100.0, "agl", true);
    const json drone = {{"id", "d"}, {"version", 2}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0}, {"cruisePowerW", 100.0},
                        {"climb", {{"method", "measured"}, {"climbPowerW", 150.0}, {"climbRateMps", 2.5}}}};
    const auto planTime = [&](const json& options) {
        const json r = analyze({{"mission", mission}, {"options", options}}, w, {}, std::optional<json>(std::in_place, drone));
        return r.at("data").at("climb").at("plan").at("timeS").get<double>();
    };
    const json cruise = {{"linkFloor", false}};
    const json fast = {{"linkFloor", false}, {"speedMps", 20.0}};
    // The level legs take half as long; only the vertical climb in the take-off zone does not halve.
    EXPECT_LT(planTime(fast), 0.6 * planTime(cruise));
    // Without a drone the note names the same airspeed the profiles use.
    const json bare = analyze({{"mission", mission}, {"options", fast}}, w, {});
    EXPECT_EQ(bare.at("data").at("climb").at("airspeedMps").get<double>(), 20.0);
    EXPECT_NE(bare.at("data").at("climb").at("notes").at(0).get<std::string>().find("assume 20 m/s"), std::string::npos);
}

TEST(ClimbAssist, OffWithTheOption) {
    const LocalWorld w = flat();
    const json r = analyze({{"mission", oneLeg(w, 2.0, 100.0, "agl", true)}, {"options", {{"climbAssist", false}}}}, w, {});
    EXPECT_FALSE(r.at("data").contains("climb"));
}

TEST(ClimbAssist, TheFinestOptionsTheSchemaAllowsStayFast) {
    // 1 m heights up to a 5000 m ceiling every 10 m over 10 km: thousands of heights per sample,
    // which a search comparing every pair of heights would take minutes over.
    const LocalWorld w = ridgeWorld();
    const json mission = oneLeg(w, 10.0, 100.0, "launch", true);
    const json options = {{"stepM", 10.0}, {"heightStepM", 1.0}, {"ceilingAglM", 5000.0}, {"linkFloor", false}};
    const json r = analyze({{"mission", mission}, {"options", options}}, w, {});
    ASSERT_TRUE(r.at("data").contains("climb")) << r.dump();
    const json& climb = r.at("data").at("climb");
    EXPECT_EQ(climb.at("strategies").size(), 3U);
    // At most 400 heights a sample: the step is coarsened, and says so.
    EXPECT_GT(climb.at("heightStepM").get<double>(), 10.0);
    bool told = false;
    for (const json& n : climb.at("notes")) told = told || n.get<std::string>().starts_with("Heights searched every");
    EXPECT_TRUE(told) << climb.at("notes").dump();
}
