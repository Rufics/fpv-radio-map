// fpv.best-launch entry: route.analyze through the host ABI, and its error paths.

#include <fake_host.hpp>
#include <fpv/search.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class BestLaunchModule : public ModuleEntryTest {
protected:
    [[nodiscard]] json request(double areaFromKm, double areaToKm) const {
        return {{"mission", eastMission(world_, 2.0, 50.0)},
                {"options", {{"area", eastArea(world_, areaFromKm, areaToKm)}, {"cellM", 100.0}, {"stepM", 500.0}}}};
    }
};

}  // namespace

TEST_F(BestLaunchModule, StandardEntry) { expectStandardEntry("fpv.best-launch"); }

TEST_F(BestLaunchModule, SearchesTheAreaWithHostLinkMargins) {
    host_.linkByDistance(30.0);
    const auto r = module_.analyze(request(0.5, 1.5));
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_FALSE(r.body.at("data").at("candidates").empty());
    EXPECT_TRUE(r.body.at("reasons").at(0).contains("fix"));
    EXPECT_GT(host_.calls("link.evaluate"), 0);
    EXPECT_GT(host_.calls("elevation.point"), 0);
}

TEST_F(BestLaunchModule, AreaBehindThePilotDoesNotBeatIt) {
    host_.linkByDistance(30.0);
    const auto r = module_.analyze(request(-1.0, -0.5));
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    const json& why = r.body.at("reasons").at(0);
    EXPECT_EQ(why.at("severity"), "info");
    EXPECT_FALSE(why.contains("fix"));
    EXPECT_NE(why.at("text").get<std::string>().find("No point in the area beats"), std::string::npos);
}

TEST_F(BestLaunchModule, AreaWithoutTerrainIsReported) {
    host_.linkByDistance(30.0);
    terrain_ = [](const LatLon& p) { return p.lon > 35.205 ? std::nullopt : std::optional<double>(100.0); };
    const auto r = module_.analyze(request(0.5, 1.5));
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "warn");
    EXPECT_TRUE(r.body.at("data").at("candidates").empty());
    EXPECT_EQ(r.body.at("data").at("notes").size(), 2u);
}

TEST_F(BestLaunchModule, FailingLinkEvaluationIsAHostError) {
    failHostCall("link.evaluate");
    const auto r = module_.analyze(request(0.5, 1.5));
    EXPECT_EQ(r.status, RFMOD_ERR_HOST_CALL);
    EXPECT_EQ(r.body.at("error").at("code"), "host_call");
}

TEST_F(BestLaunchModule, TooFineAGridIsABadRequest) {
    host_.linkByDistance(30.0);
    json req = request(0.5, 1.5);
    req["options"]["cellM"] = 10.0;
    req["options"]["maxCandidates"] = 5;
    const auto r = module_.analyze(req);
    EXPECT_EQ(r.status, RFMOD_ERR_BAD_REQUEST);
    EXPECT_NE(r.body.at("error").at("message").get<std::string>().find("more than 5"), std::string::npos);
}

TEST_F(BestLaunchModule, DroneBelowTheGroundIsEvaluatedOnTheGround) {
    // a launch-referenced plan under a ridge gives a negative AGL; link.evaluate takes none
    host_.linkByDistance(30.0);
    const rfmod::HostClient client(host_.abi());
    const HostLinkEvaluator eval(client, json::object());
    const LatLon pilot{48.53, 35.2};
    (void)eval.marginDb("video", {pilot, 2.0}, {{48.53, 35.21}, -40.0});
    EXPECT_EQ(host_.requests().back().at("request").at("drone").at("aglM"), 0.0);
}
