// fpv.airframe entry: route.analyze through the host ABI and the library lookup.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class AirframeModule : public ModuleEntryTest {
protected:
    void SetUp() override {
        ModuleEntryTest::SetUp();
        host_.libraryItem("drones", "wing", {{"id", "wing"}, {"airframe", "fixed-wing"}, {"cruiseAirspeedMps", 18.0},
                                             {"fixedWing", {{"stallSpeedMps", 10.0}, {"maxBankDeg", 30.0},
                                                            {"maxClimbAngleDeg", 12.0}, {"maxDiveAngleDeg", 10.0}}}});
    }
};

}  // namespace

TEST_F(AirframeModule, StandardEntry) { expectStandardEntry("fpv.airframe"); }

TEST_F(AirframeModule, AsksForADroneFirst) {
    const auto r = module_.analyze({{"mission", eastMission(world_, 2.0, 50.0)}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "info");
    EXPECT_EQ(host_.calls("library.get"), 0);
}

TEST_F(AirframeModule, LooksUpTheDroneAndChecksTheRoute) {
    json m = eastMission(world_, 2.0, 50.0, "none", true);
    m["drone"] = "wing";
    const auto r = module_.analyze({{"mission", m}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(r.body.at("data").at("airframe"), "fixed-wing");
    EXPECT_EQ(r.body.at("data").at("turns").size(), 1u);
    EXPECT_EQ(host_.calls("library.get"), 1);
    EXPECT_GT(host_.calls("profile.get"), 0);
}
