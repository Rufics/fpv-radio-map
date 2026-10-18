// fpv.core entry: route.analyze through the host ABI, and its error paths.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class CoreModule : public ModuleEntryTest {};

}  // namespace

TEST_F(CoreModule, StandardEntry) { expectStandardEntry("fpv.core"); }

TEST_F(CoreModule, ComparesAndExportsWithHostAnswers) {
    host_.linkByDistance(10.0);
    json mission = eastMission(world_, 1.0, 50.0);
    mission["name"] = "Host run";
    const json req = {{"mission", mission},
                      {"options", {{"compare", {{"name", "Higher"}, {"patch", {{"waypoints", {{{"lat", 48.5}, {"lon", 35.21}, {"aglM", 80.0}}}}}}}},
                                   {"stepM", 500.0},
                                   {"export", {"kml", "gpx"}}}}};
    const auto r = module_.analyze(req);
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    const json& data = r.body.at("data");
    EXPECT_TRUE(data.at("compare").at("links").contains("control"));
    EXPECT_NE(data.at("export").at("kml").at("content").get<std::string>().find("Host run"), std::string::npos);
    EXPECT_NE(data.at("export").at("gpx").at("content").get<std::string>().find("<trkpt"), std::string::npos);
    // Each scenario evaluates its own mission document through the host.
    bool sawPatched = false;
    for (const json& call : host_.requests()) {
        if (call.at("method") == "link.evaluate") {
            sawPatched = sawPatched || call.at("request").at("mission").at("waypoints").at(0).at("aglM") == 80.0;
        }
    }
    EXPECT_TRUE(sawPatched);
}

TEST_F(CoreModule, MissingTerrainForAbsoluteExportIsAWarning) {
    terrain_ = [](const LatLon&) { return std::optional<double>(); };
    const json req = {{"mission", eastMission(world_, 1.0, 50.0)}, {"options", {{"export", {"kml"}}, {"kmlAltitude", "absolute"}}}};
    const auto r = module_.analyze(req);
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    ASSERT_EQ(r.body.at("reasons").size(), 1u);
    // OpenAPI Reason.severity is one of info, warn, blocked.
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "warn");
    EXPECT_FALSE(r.body.at("data").contains("export"));
}

TEST_F(CoreModule, UnknownExportFormatIsABadRequest) {
    const auto r = module_.analyze({{"mission", eastMission(world_, 1.0, 50.0)}, {"options", {{"export", {"shp"}}}}});
    EXPECT_EQ(r.status, RFMOD_ERR_BAD_REQUEST);
    EXPECT_NE(r.body.at("error").at("message").get<std::string>().find("shp"), std::string::npos);
}
