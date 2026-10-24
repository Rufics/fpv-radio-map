#include "core.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace fpv;

namespace {

class DistanceLink final : public ILinkEvaluator {
public:
    DistanceLink(const IWorld& w, double gainDb) : w_(w), gainDb_(gainDb) {}
    double marginDb(const std::string&, const End& g, const End& d) const override {
        return 10.0 + gainDb_ - w_.inverse(g.position, d.position).distanceM / 100.0;
    }

private:
    const IWorld& w_;
    double gainDb_;
};

const LocalWorld& world() {
    static const LocalWorld w([](const LatLon& p) { return std::optional<double>(100.0 + (p.lon - 35.2) * 1000.0); }, 30.0);
    return w;
}

json missionTo(double distanceM, bool back) {
    const LatLon a{48.5, 35.2};
    const LatLon t = world().direct(a, 90.0, distanceM).point;
    return {{"name", "M"}, {"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}, {"label", "Target"}}}},
            {"returnToPilot", back}, {"evaluate", "both"}};
}

auto factory() {
    return [](const json& m) -> std::unique_ptr<ILinkEvaluator> {
        return std::make_unique<DistanceLink>(world(), m.value("gainDb", 0.0));
    };
}

}  // namespace

TEST(Core, ComparePointByPoint) {
    const json r = core::analyze({{"mission", missionTo(1000.0, false)},
                                  {"options", {{"compare", {{"patch", {{"gainDb", -2.5}}}}}, {"stepM", 250.0}}}},
                                 world(), factory());
    const json& c = r.at("data").at("compare");
    EXPECT_TRUE(c.at("aligned").get<bool>());
    for (const char* link : {"control", "video"}) {
        const json& l = c.at("links").at(link);
        EXPECT_NEAR(l.at("a").at("worstMarginDb").get<double>(), 0.0, 1e-6);
        EXPECT_NEAR(l.at("deltaWorstDb").get<double>(), -2.5, 1e-12);
        ASSERT_EQ(l.at("points").size(), 4u);
        for (const json& p : l.at("points")) EXPECT_NEAR(p.at("deltaDb").get<double>(), -2.5, 1e-12);
        EXPECT_EQ(l.at("pointsWorse"), 4);
    }
    EXPECT_EQ(r.at("reasons").size(), 2u);
}

TEST(Core, DifferentRoutesCompareOnlyWorst) {
    const json patch = {{"waypoints", missionTo(2000.0, false).at("waypoints")}};
    const json r = core::analyze({{"mission", missionTo(1000.0, false)}, {"options", {{"compare", {{"patch", patch}}}}}},
                                 world(), factory());
    const json& c = r.at("data").at("compare");
    EXPECT_FALSE(c.at("aligned").get<bool>());
    EXPECT_FALSE(c.at("links").at("control").contains("points"));
    EXPECT_NEAR(c.at("links").at("control").at("deltaWorstDb").get<double>(), -10.0, 1e-6);
}

TEST(Core, ExportVerticesAreExactAndIncludeEnds) {
    const Mission m = Mission::fromJson(missionTo(1000.0, true));
    const Route route(m, world());
    const auto v = core::exportVertices(route, 300.0);
    // Out: 0, 300, 600, 900; back: 1000, 1300, 1600, 1900; end at pilot.
    ASSERT_EQ(v.size(), 9u);
    EXPECT_DOUBLE_EQ(v.front().aglM, 1.5);
    EXPECT_DOUBLE_EQ(v[4].aglM, 50.0);
    EXPECT_DOUBLE_EQ(v.back().aglM, 1.5);
    EXPECT_NEAR(v[1].aglM, 1.5 + 0.3 * 48.5, 1e-9);
    EXPECT_NEAR(world().inverse(v[0].position, v[1].position).distanceM, 300.0, 1e-6);
}

TEST(Core, KmlAndGpx) {
    json mission = missionTo(1000.0, true);
    mission["name"] = "Ridge \"north\" & back";
    const json r = core::analyze({{"mission", mission},
                                  {"options", {{"export", {"kml", "gpx"}}, {"kmlAltitude", "absolute"}, {"exportStepM", 500.0}}}},
                                 world(), factory());
    const std::string kml = r.at("data").at("export").at("kml").at("content");
    const std::string gpx = r.at("data").at("export").at("gpx").at("content");
    EXPECT_NE(kml.find("<name>Ridge &quot;north&quot; &amp; back</name>"), std::string::npos);
    EXPECT_NE(kml.find("<altitudeMode>absolute</altitudeMode>"), std::string::npos);
    EXPECT_NE(kml.find("35.200000000,48.500000000,101.500"), std::string::npos);
    EXPECT_NE(gpx.find("<wpt lat=\"48.500000000\" lon=\"35.200000000\"><ele>101.500</ele><name>Pilot</name>"), std::string::npos);
    EXPECT_NE(gpx.find("<name>Target</name>"), std::string::npos);
    // rte: pilot, target, pilot.
    std::size_t n = 0;
    for (std::size_t p = gpx.find("<rtept"); p != std::string::npos; p = gpx.find("<rtept", p + 1)) ++n;
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(r.at("data").at("export").at("gpx").at("mediaType"), "application/gpx+xml");
}

TEST(Core, RelativeKmlKeepsAgl) {
    const json r = core::analyze({{"mission", missionTo(1000.0, false)}, {"options", {{"export", {"kml"}}}}}, world(), factory());
    const std::string kml = r.at("data").at("export").at("kml").at("content");
    EXPECT_NE(kml.find("<altitudeMode>relativeToGround</altitudeMode>"), std::string::npos);
    EXPECT_NE(kml.find("35.200000000,48.500000000,1.500"), std::string::npos);
}

TEST(Core, UnknownFormatIsRejected) {
    EXPECT_THROW(core::analyze({{"mission", missionTo(1000.0, false)}, {"options", {{"export", {"shp"}}}}}, world(), factory()),
                 rfmod::BadRequest);
}

TEST(Core, LaunchReferencedRouteHoldsItsAltitudeOverTheTerrain) {
    // The ground rises eastwards; above the launch point the drone does not follow it.
    json mission = missionTo(1000.0, false);
    mission["altitudeRef"] = "launch";
    mission["waypoints"][0].erase("aglM");
    mission["waypoints"][0]["altM"] = 50.0;
    const Mission m = Mission::fromJson(mission);
    const Route route(m, world());
    const RoutePoint mid = route.at(500.0);
    EXPECT_NEAR(mid.heightM, 1.5 + 0.5 * 48.5, 1e-9);
    const double launchGround = world().groundMsl(m.pilot.position);
    EXPECT_NEAR(mid.aglM, launchGround + mid.heightM - world().groundMsl(mid.position), 1e-9);
    EXPECT_LT(mid.aglM, mid.heightM);
    // Exports carry the altitude above sea level and the height above the ground under the point.
    const json r = core::analyze({{"mission", mission}, {"options", {{"export", {"gpx"}}}}}, world(), factory());
    const std::string gpx = r.at("data").at("export").at("gpx").at("content");
    const LatLon t{mission["waypoints"][0]["lat"].get<double>(), mission["waypoints"][0]["lon"].get<double>()};
    const double tGround = world().groundMsl(t);
    EXPECT_NE(gpx.find("<ele>" + fixed(launchGround + 50.0, 3) + "</ele><name>Target</name><desc>" +
                       fixed(launchGround + 50.0 - tGround, 3) + " m above ground"),
              std::string::npos)
        << gpx;
}
