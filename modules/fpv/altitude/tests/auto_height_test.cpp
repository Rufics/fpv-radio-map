// route.prepare: automatic waypoint heights from the corridor floor.

#include "auto_height.hpp"

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::altitude;
using namespace fpv::test;

namespace {

const LatLon kPilot{48.5, 35.2};

LocalWorld worldOf(std::function<double(double)> groundByEastM) {
    return LocalWorld([g = std::move(groundByEastM)](const LatLon& p) { return std::optional<double>(g((p.lon - kPilot.lon) * 73800.0)); },
                      10.0);
}

/// Waypoints due east at `kms`; a negative height leaves it to the server.
json eastRoute(const IWorld& world, const std::vector<std::pair<double, double>>& kms, const std::string& ref = "agl") {
    json wps = json::array();
    for (const auto& [km, h] : kms) {
        const LatLon p = world.direct(kPilot, 90.0, km * 1000.0).point;
        json w = {{"lat", p.lat}, {"lon", p.lon}, {"label", "W" + std::to_string(wps.size() + 1)}};
        if (h >= 0.0) w[ref == "launch" ? "altM" : "aglM"] = h;
        wps.push_back(w);
    }
    return {{"altitudeRef", ref},
            {"pilot", {{"lat", kPilot.lat}, {"lon", kPilot.lon}, {"aglM", 1.5}}},
            {"waypoints", wps},
            {"returnToPilot", false}};
}

json run(const IWorld& world, const json& mission, const json& options = json::object(), const LinkInputs& links = {}) {
    return prepare({{"mission", mission}, {"options", options}}, world, links);
}

}  // namespace

TEST(AutoHeight, OnFlatGroundTheWaypointFliesAtTheClearance) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    const json r = run(w, eastRoute(w, {{1.0, -1.0}}), {{"stepM", 100.0}});
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    const json& h = r.at("waypoints")[0];
    EXPECT_EQ(h.at("index"), 0);
    EXPECT_EQ(h.at("field"), "aglM");
    EXPECT_DOUBLE_EQ(h.at("heightM").get<double>(), 30.0);
    EXPECT_EQ(h.at("limitedBy"), "terrain");
    EXPECT_FALSE(h.at("blocked").get<bool>());
    EXPECT_TRUE(r.at("reasons").empty());
}

TEST(AutoHeight, TwoFreeEndsFlyLevelOverARidge) {
    // 60 m ridge half way between two automatic waypoints, flat ground at 100 m elsewhere. (An AGL
    // plan follows the ground, so only a launch-referenced one has to climb over it.)
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1500.0) < 60.0 ? 160.0 : 100.0; });
    const json r = run(w, eastRoute(w, {{1.0, -1.0}, {2.0, -1.0}}, "launch"), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("waypoints").size(), 2U);
    for (const json& h : r.at("waypoints")) {
        EXPECT_GE(h.at("heightM").get<double>(), 90.0) << h.dump();
        EXPECT_LE(h.at("heightM").get<double>(), 91.0) << h.dump();
    }
    EXPECT_EQ(r.at("waypoints")[1].at("limitedBy"), "terrain");
}

TEST(AutoHeight, AManualHeightIsNeverChangedAndCarriesTheFreeEnd) {
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1500.0) < 60.0 ? 160.0 : 100.0; });
    // W1 manual at 80 m: only W2 can lift the leg over the ridge (90 m above launch, from 42 % along).
    const json r = run(w, eastRoute(w, {{1.0, 80.0}, {2.0, -1.0}}, "launch"), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    const json& h = r.at("waypoints")[0];
    EXPECT_EQ(h.at("index"), 1);
    EXPECT_GE(h.at("heightM").get<double>(), 100.0) << h.dump();
    EXPECT_LE(h.at("heightM").get<double>(), 110.0) << h.dump();
}

TEST(AutoHeight, HeightSourceAutoReplacesAStoredHeight) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    json m = eastRoute(w, {{1.0, 75.0}});
    m["waypoints"][0]["heightSource"] = "auto";
    const json r = run(w, m);
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    EXPECT_DOUBLE_EQ(r.at("waypoints")[0].at("heightM").get<double>(), 30.0);
}

TEST(AutoHeight, OverTheCeilingTheWaypointIsBlocked) {
    // A 150 m hill before the waypoint: clearing it takes 180 m above launch, over the 120 m ceiling
    // at the waypoint on the plain.
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1000.0) < 300.0 ? 100.0 + 150.0 * (1.0 - std::abs(x - 1000.0) / 300.0) : 100.0; });
    const json r = run(w, eastRoute(w, {{1.4, -1.0}}, "launch"), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    const json& h = r.at("waypoints")[0];
    EXPECT_TRUE(h.at("blocked").get<bool>()) << h.dump();
    EXPECT_EQ(h.at("limitedBy"), "ceiling");
    EXPECT_DOUBLE_EQ(h.at("heightM").get<double>(), 120.0);
    ASSERT_EQ(r.at("reasons").size(), 1U);
    const json& why = r.at("reasons")[0];
    EXPECT_EQ(why.at("severity"), "warn");
    EXPECT_EQ(why.at("leg"), 0);
    const std::string text = why.at("text");
    EXPECT_NE(text.find("W1"), std::string::npos) << text;
    EXPECT_NE(text.find("fpv.relay"), std::string::npos) << text;
    EXPECT_NE(text.find("fpv.best-launch"), std::string::npos) << text;
}

TEST(AutoHeight, AWholeMetreHeightStaysUnderAnUnevenCeiling) {
    // Launch at 100 m, the plain beyond at 100.4 m: the ceiling at the waypoint is 120.4 m above launch.
    const LocalWorld w = worldOf([](double x) {
        if (x < 50.0) return 100.0;
        return std::abs(x - 1000.0) < 300.0 ? 100.4 + 150.0 * (1.0 - std::abs(x - 1000.0) / 300.0) : 100.4;
    });
    const json r = run(w, eastRoute(w, {{1.4, -1.0}}, "launch"), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    EXPECT_TRUE(r.at("waypoints")[0].at("blocked").get<bool>());
    EXPECT_DOUBLE_EQ(r.at("waypoints")[0].at("heightM").get<double>(), 120.0) << "rounded down, not up past the ceiling";
}

TEST(AutoHeight, ALegOverTheCeilingBetweenTwoHillsBlocksBoth) {
    // Two waypoints on 200 m hills with the plain at 100 m between them: the level leg 130 m above
    // launch is 130 m AGL over the plain, over the ceiling though both waypoints are under theirs.
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1000.0) < 100.0 || std::abs(x - 2000.0) < 100.0 ? 200.0 : 100.0; });
    const json r = run(w, eastRoute(w, {{1.0, -1.0}, {2.0, -1.0}}, "launch"), {{"stepM", 50.0}});
    ASSERT_EQ(r.at("waypoints").size(), 2U);
    for (const json& h : r.at("waypoints")) {
        EXPECT_TRUE(h.at("blocked").get<bool>()) << h.dump();
        EXPECT_EQ(h.at("limitedBy"), "ceiling") << h.dump();
        EXPECT_DOUBLE_EQ(h.at("heightM").get<double>(), 130.0) << h.dump();
    }
    EXPECT_EQ(r.at("reasons").size(), 2U);
}

TEST(AutoHeight, ALinkOverARidgeSetsTheHeight) {
    // A 30 m ridge half way: the control link needs more than the 30 m terrain clearance at the waypoint.
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1000.0) < 100.0 ? 130.0 : 100.0; });
    const LinkInputs links{{"control", {rfcore::Hertz(915e6), "ELRS 900"}}};
    const json r = run(w, eastRoute(w, {{2.0, -1.0}}), {{"stepM", 100.0}}, links);
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    const json& h = r.at("waypoints")[0];
    EXPECT_EQ(h.at("limitedBy"), "control") << h.dump();
    EXPECT_GT(h.at("heightM").get<double>(), 30.0);
    EXPECT_FALSE(h.at("blocked").get<bool>());
}

TEST(AutoHeight, ALinkNeedOverTheCeilingAtTheLastWaypointIsCapped) {
    // A 60 m ridge half way: the control link would need about 148 m AGL at the route's last point.
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1000.0) < 100.0 ? 160.0 : 100.0; });
    const LinkInputs links{{"control", {rfcore::Hertz(915e6), "ELRS 900"}}};
    const json r = run(w, eastRoute(w, {{2.0, -1.0}}), {{"stepM", 100.0}}, links);
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    const json& h = r.at("waypoints")[0];
    EXPECT_DOUBLE_EQ(h.at("heightM").get<double>(), 120.0) << h.dump();
    EXPECT_EQ(h.at("limitedBy"), "ceiling");
    EXPECT_TRUE(h.at("blocked").get<bool>());
}

TEST(AutoHeight, NoClearLinkHeightBlocksTheWaypoint) {
    // A 60 m ridge half way: no height up to maxAglM clears the link, though the terrain is easy.
    const LocalWorld w = worldOf([](double x) { return std::abs(x - 1000.0) < 100.0 ? 160.0 : 100.0; });
    const LinkInputs links{{"control", {rfcore::Hertz(915e6), "ELRS 900"}}};
    const json r = run(w, eastRoute(w, {{2.0, -1.0}}), {{"stepM", 100.0}, {"maxAglM", 100.0}}, links);
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    EXPECT_TRUE(r.at("waypoints")[0].at("blocked").get<bool>()) << r.dump();
    ASSERT_EQ(r.at("reasons").size(), 1U);
    const std::string text = r.at("reasons")[0].at("text");
    EXPECT_NE(text.find("Fresnel zone clear"), std::string::npos) << text;
}

TEST(AutoHeight, ALaunchMissionGetsLaunchHeights) {
    // Ground rising to 150 m at the waypoint, launch at 100 m: 50 + 30 above launch.
    const LocalWorld w = worldOf([](double x) { return 100.0 + x / 20.0; });
    const json r = run(w, eastRoute(w, {{1.0, -1.0}}, "launch"), {{"stepM", 100.0}});
    ASSERT_EQ(r.at("waypoints").size(), 1U);
    EXPECT_EQ(r.at("waypoints")[0].at("field"), "altM");
    EXPECT_NEAR(r.at("waypoints")[0].at("heightM").get<double>(), 80.0, 1.0);
}

TEST(AutoHeight, NothingToDoWhenOffOrAllManual) {
    const LocalWorld w = worldOf([](double) { return 100.0; });
    EXPECT_TRUE(run(w, eastRoute(w, {{1.0, -1.0}}), {{"autoHeight", false}}).at("waypoints").empty());
    EXPECT_TRUE(run(w, eastRoute(w, {{1.0, 60.0}})).at("waypoints").empty());
}

TEST(AutoHeight, MissingTerrainIsAReasonNotAnError) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(); }, 10.0);
    const json r = run(w, eastRoute(w, {{1.0, -1.0}}, "launch"));
    EXPECT_TRUE(r.at("waypoints").empty());
    ASSERT_EQ(r.at("reasons").size(), 1U);
    EXPECT_NE(r.at("reasons")[0].at("text").get<std::string>().find("Automatic heights not set"), std::string::npos);
}

TEST_F(ModuleEntryTest, RoutePrepareGoesThroughTheEntry) {
    json m = eastMission(world_, 1.0, 60.0);
    m["waypoints"][0].erase("aglM");
    const auto r = module_.invoke("route.prepare", {{"mission", m}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    ASSERT_EQ(r.body.at("waypoints").size(), 1U) << r.body.dump();
    EXPECT_DOUBLE_EQ(r.body.at("waypoints")[0].at("heightM").get<double>(), 30.0);
}
