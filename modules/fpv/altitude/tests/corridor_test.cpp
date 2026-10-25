#include "corridor.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace fpv;
using namespace fpv::altitude;

namespace {

const LatLon kPilot{48.5, 35.2};

LocalWorld worldOf(std::function<double(double)> groundByEastM) {
    return LocalWorld([g = std::move(groundByEastM)](const LatLon& p) { return std::optional<double>(g((p.lon - kPilot.lon) * 73800.0)); },
                      10.0);
}

/// Due east to `km`, at `height` in the mission's reference at the end. With `climbOutKm` the
/// drone is already at that height that far out, as after a steep climb-out.
json eastLeg(const IWorld& world, double km, double height, const std::string& ref = "launch", double climbOutKm = 0.0) {
    const auto wp = [&world, &ref, height](double atKm, const char* label) {
        const LatLon p = world.direct(kPilot, 90.0, atKm * 1000.0).point;
        json w = {{"lat", p.lat}, {"lon", p.lon}, {"label", label}};
        w[ref == "launch" ? "altM" : "aglM"] = height;
        return w;
    };
    json wps = json::array();
    if (climbOutKm > 0.0) wps.push_back(wp(climbOutKm, "C"));
    wps.push_back(wp(km, "B"));
    return {{"altitudeRef", ref},
            {"pilot", {{"lat", kPilot.lat}, {"lon", kPilot.lon}, {"aglM", 1.5}}},
            {"waypoints", wps},
            {"returnToPilot", false}};
}

/// The corridor alone: the climb assistant's advice is tested in climb_test.cpp.
json run(const IWorld& world, const json& mission, const json& options = json::object(), const LinkInputs& links = {}) {
    json o = options;
    if (!o.contains("climbAssist")) o["climbAssist"] = false;
    return analyze({{"mission", mission}, {"options", o}}, world, links);
}

double back(const json& r, const char* key) { return r.at("data").at("corridor").at(key).back().get<double>(); }

}  // namespace

TEST(LegGround, InterpolatesAndTakesTheHighestObstacle) {
    const LegGround g({{0.0, 100.0, 0.0, {}}, {10.0, 110.0, 25.0, {}}, {20.0, 90.0, 0.0, {}}});
    EXPECT_DOUBLE_EQ(g.groundAt(5.0), 105.0);
    EXPECT_DOUBLE_EQ(g.groundAt(-3.0), 100.0);
    EXPECT_DOUBLE_EQ(g.groundAt(30.0), 90.0);
    EXPECT_DOUBLE_EQ(g.highestIn(5.0, 15.0), 135.0) << "the tree on the middle sample";
    EXPECT_DOUBLE_EQ(g.highestIn(12.0, 20.0), 106.0) << "no sample inside: the higher end";
}

TEST(Corridor, SamplesEveryStepAndEveryWaypoint) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    json m = eastLeg(w, 1.05, 50.0);
    const LatLon c = w.direct(kPilot, 0.0, 400.0).point;
    m["waypoints"].push_back({{"lat", c.lat}, {"lon", c.lon}, {"altM", 50.0}});
    const Route route(Mission::fromJson(m), w);
    const auto s = sampleDistances(route, 500.0);
    // 0, 500, 1000, the first waypoint at 1050, 1500, 2000 and the end at about 2173.
    ASSERT_EQ(s.size(), 7U);
    EXPECT_DOUBLE_EQ(s[0], 0.0);
    EXPECT_DOUBLE_EQ(s[1], 500.0);
    EXPECT_NEAR(s[3], route.legs()[0].lengthM, 1e-9);
    EXPECT_DOUBLE_EQ(s[5], 2000.0);
    EXPECT_NEAR(s[6], route.length(), 1e-9);
}

TEST(Corridor, FloorAndCeilingFollowTheGroundWhileALaunchPlanFliesLevel) {
    // Ground rising 100 m over 2 km, a level leg 150 m above launch.
    const LocalWorld w = worldOf([](double x) { return 100.0 + x / 20.0; });
    const json r = run(w, eastLeg(w, 2.0, 150.0, "launch", 0.2), {{"stepM", 250.0}, {"ceilingAglM", 200.0}});
    EXPECT_NEAR(back(r, "groundMslM"), 200.0, 0.5);
    EXPECT_NEAR(back(r, "floorMslM"), 230.0, 0.5) << "the highest ground within half a step, plus 30 m";
    EXPECT_DOUBLE_EQ(back(r, "ceilingMslM"), back(r, "groundMslM") + 200.0);
    EXPECT_NEAR(back(r, "plannedMslM"), 250.0, 1e-6);
    EXPECT_EQ(r.at("data").at("reference"), "launch");
    // Both heights of the waypoint, for reference.
    const json& b = r.at("data").at("waypoints").back();
    EXPECT_NEAR(b.at("aboveLaunchM").get<double>(), 150.0, 1e-6);
    EXPECT_NEAR(b.at("aglM").get<double>(), 50.0, 0.5);
    EXPECT_TRUE(r.at("reasons").empty()) << r.at("reasons").dump();
}

TEST(Corridor, AnAglPlanFollowsTheGround) {
    const LocalWorld w = worldOf([](double x) { return 100.0 + x / 20.0; });
    const json r = run(w, eastLeg(w, 2.0, 60.0, "agl"));
    const json& c = r.at("data").at("corridor");
    for (std::size_t i = 3; i < c.at("s").size(); ++i) {
        EXPECT_NEAR(c.at("plannedMslM")[i].get<double>() - c.at("groundMslM")[i].get<double>(), 1.5 + (60.0 - 1.5) * c.at("s")[i].get<double>() / 2000.0, 0.2);
    }
    EXPECT_EQ(r.at("data").at("reference"), "agl");
}

TEST(Corridor, WarnsWhereTheRouteDropsUnderTheFloor) {
    // A 60 m wall at 1 km; the plan is 50 m above launch, so 40 m under the floor there.
    const LocalWorld w = worldOf([](double x) { return x > 980.0 && x < 1020.0 ? 160.0 : 100.0; });
    const json r = run(w, eastLeg(w, 2.0, 50.0, "launch", 0.2), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("reasons").size(), 1U) << r.at("reasons").dump();
    const json& why = r.at("reasons").at(0);
    EXPECT_EQ(why.at("severity"), "warn");
    EXPECT_EQ(why.at("leg"), 1);
    EXPECT_NE(why.at("text").get<std::string>().find("under the safe corridor at km 1.00"), std::string::npos) << why;
    EXPECT_NE(why.at("text").get<std::string>().find("30 m above the ground and obstacles"), std::string::npos) << why;
    EXPECT_NEAR(r.at("data").at("summary").at("lowestMarginM").get<double>(), 150.0 - 190.0, 1e-6);
    EXPECT_NEAR(r.at("data").at("summary").at("lowestMarginAtM").get<double>(), 1000.0, 1e-6);
}

TEST(Corridor, TheTakeOffZoneIsNotWarnedAbout) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    // Climbing to 40 m above launch over 1 km: 30 m is reached only at 740 m out.
    EXPECT_TRUE(run(w, eastLeg(w, 1.0, 40.0), {{"takeoffM", 800.0}}).at("reasons").empty());
    const json r = run(w, eastLeg(w, 1.0, 40.0), {{"takeoffM", 200.0}});
    ASSERT_EQ(r.at("reasons").size(), 1U);
    // The floor ramps up from the ground at the pilot.
    const json& c = r.at("data").at("corridor");
    EXPECT_NEAR(c.at("floorMslM")[0].get<double>(), 100.0, 1e-9);
    EXPECT_NEAR(c.at("floorMslM")[1].get<double>(), 115.0, 0.1);
}

TEST(Corridor, WarnsOverTheCeiling) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    const json r = run(w, eastLeg(w, 1.0, 200.0), {{"ceilingAglM", 150.0}});
    ASSERT_EQ(r.at("reasons").size(), 1U);
    EXPECT_NE(r.at("reasons").at(0).at("text").get<std::string>().find("200 m above the ground at km 1.00, over the 150 m ceiling"), std::string::npos)
        << r.at("reasons");
}

TEST(Corridor, TheLinkFloorRisesBehindAHill) {
    // A 40 m hill at 500 m; past it the link needs more than the terrain clearance.
    const LocalWorld w = worldOf([](double x) { return x > 480.0 && x < 520.0 ? 140.0 : 100.0; });
    const LinkInputs links{{"control", {rfcore::Hertz(915e6), "ELRS 900"}}};
    const json r = run(w, eastLeg(w, 3.0, 50.0, "launch", 0.2), {{"stepM", 250.0}}, links);
    const json& c = r.at("data").at("corridor");
    EXPECT_GT(back(r, "linkFloorMslM"), back(r, "terrainFloorMslM"));
    EXPECT_DOUBLE_EQ(back(r, "floorMslM"), back(r, "linkFloorMslM"));
    EXPECT_TRUE(c.at("linkFloorMslM")[0].is_null()) << "nothing to clear at the pilot";
    EXPECT_EQ(r.at("data").at("fresnel").at("link"), "control");
    const double lambda = 299792458.0 / 915e6;
    EXPECT_NEAR(back(r, "fresnelRadiusM"), 0.5 * std::sqrt(lambda * 3000.0), 0.05);
    ASSERT_EQ(r.at("reasons").size(), 1U) << r.at("reasons");
    EXPECT_NE(r.at("reasons").at(0).at("text").get<std::string>().find("60 % of the control Fresnel zone clear"), std::string::npos)
        << r.at("reasons");

    const json off = run(w, eastLeg(w, 3.0, 50.0, "launch", 0.2), {{"stepM", 250.0}, {"linkFloor", false}}, links);
    // Without the link floor the hill's own clearance is what the plan misses.
    ASSERT_EQ(off.at("reasons").size(), 1U) << off.at("reasons");
    EXPECT_NE(off.at("reasons").at(0).at("text").get<std::string>().find("30 m above the ground and obstacles"), std::string::npos);
    for (const json& v : off.at("data").at("corridor").at("linkFloorMslM")) EXPECT_TRUE(v.is_null());
    EXPECT_DOUBLE_EQ(back(off, "floorMslM"), back(off, "terrainFloorMslM"));
}

TEST(Corridor, MissingTerrainIsAWarningNotAFailure) {
    const LocalWorld w([](const LatLon& p) { return p.lon > 35.21 ? std::nullopt : std::optional<double>(100.0); }, 10.0);
    const json r = run(w, eastLeg(w, 2.0, 50.0));
    EXPECT_FALSE(r.at("data").contains("corridor"));
    ASSERT_EQ(r.at("reasons").size(), 1U);
    EXPECT_NE(r.at("reasons").at(0).at("text").get<std::string>().find("Altitude corridor not drawn"), std::string::npos);
}

TEST(Settings, OptionsAreKeptInsideTheSchemaBounds) {
    const Settings s = Settings::fromOptions({{"stepM", 0.0}, {"clearanceM", -5.0}, {"takeoffM", 1e9},
                                              {"fresnelFraction", 0.0}, {"maxAglM", 7.0}, {"ceilingAglM", 300.0}});
    EXPECT_DOUBLE_EQ(s.stepM, 10.0) << "a zero step would never end the sampling";
    EXPECT_DOUBLE_EQ(s.clearanceM, 0.0);
    EXPECT_DOUBLE_EQ(s.takeoffM, 5000.0);
    EXPECT_DOUBLE_EQ(s.fraction, 0.01);
    EXPECT_DOUBLE_EQ(s.maxAglM, 10.0);
    EXPECT_DOUBLE_EQ(s.ceilingAglM, 300.0);
    EXPECT_DOUBLE_EQ(Settings::fromOptions(json::object()).stepM, 100.0);
}
