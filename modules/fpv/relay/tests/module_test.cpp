// fpv.relay entry: route.analyze through the host ABI, and its error paths.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class RelayModule : public ModuleEntryTest {};

}  // namespace

TEST_F(RelayModule, StandardEntry) { expectStandardEntry("fpv.relay"); }

TEST_F(RelayModule, ChosenRelayAndAreaSearchUseHostMargins) {
    host_.linkByDistance(10.0);
    const LatLon near = world_.direct({48.5, 35.2}, 90.0, 300.0).point;
    const json req = {{"mission", eastMission(world_, 2.0, 50.0)},
                      {"options", {{"relay", {{"lat", near.lat}, {"lon", near.lon}}},
                                   {"area", eastArea(world_, 0.5, 1.5)},
                                   {"cellM", 100.0},
                                   {"stepM", 500.0}}}};
    const auto r = module_.analyze(req);
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_TRUE(r.body.at("data").contains("chosen"));
    EXPECT_FALSE(r.body.at("data").at("candidates").empty());
    // The relay 300 m out leaves 1.7 km to the target (-7 dB); the area's middle does better.
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "blocked");
    EXPECT_TRUE(r.body.at("reasons").back().contains("fix"));
    EXPECT_GT(host_.calls("link.evaluate"), 0);
}

TEST_F(RelayModule, FailingGeodesyIsAHostError) {
    host_.linkByDistance(10.0);
    failHostCall("geodesy.inverse");
    const auto r = module_.analyze({{"mission", eastMission(world_, 2.0, 50.0)}});
    EXPECT_EQ(r.status, RFMOD_ERR_HOST_CALL);
    EXPECT_NE(r.body.at("error").at("message").get<std::string>().find("geodesy.inverse is down"), std::string::npos);
}
