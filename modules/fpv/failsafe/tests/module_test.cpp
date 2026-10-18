// fpv.failsafe entry: route.analyze through the host ABI, library lookups and error paths.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class FailsafeModule : public ModuleEntryTest {
protected:
    void SetUp() override {
        ModuleEntryTest::SetUp();
        // Control is lost 2 km from the pilot on a 3 km route.
        host_.linkByDistance(20.0);
        host_.libraryItem("drones", "quad", {{"id", "quad"}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0},
                                             {"cruisePowerW", 100.0}, {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}});
        host_.libraryItem("batteries", "pack", {{"id", "pack"}, {"cellsSeries", 6}, {"capacityAh", 1.5},
                                                {"nominalCellVoltage", 3.7}, {"usableFraction", 0.8}});
    }

    [[nodiscard]] json withEquipment(const std::string& battery) const {
        json m = eastMission(world_, 3.0, 50.0, "control", true);
        m["drone"] = "quad";
        m["battery"] = battery;
        return m;
    }
};

}  // namespace

TEST_F(FailsafeModule, StandardEntry) { expectStandardEntry("fpv.failsafe"); }

TEST_F(FailsafeModule, LooksUpTheDroneAndBatteryForTheEnergyCheck) {
    const auto r = module_.analyze({{"mission", withEquipment("pack")}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    const json& rth = r.body.at("data").at("rth");
    EXPECT_NEAR(r.body.at("data").at("loss").at("distanceAlongRouteM").get<double>(), 2000.0, 1.0);
    EXPECT_TRUE(rth.contains("energyWh"));
    EXPECT_TRUE(rth.contains("usableWh"));
    EXPECT_EQ(host_.calls("library.get"), 2);
    EXPECT_GT(host_.calls("profile.get"), 0);
}

TEST_F(FailsafeModule, WithoutEquipmentNoLibraryCall) {
    const auto r = module_.analyze({{"mission", eastMission(world_, 3.0, 50.0, "control", true)}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_FALSE(r.body.at("data").at("rth").contains("energyWh"));
    EXPECT_EQ(host_.calls("library.get"), 0);
}

TEST_F(FailsafeModule, UnknownLibraryItemIsAHostError) {
    const auto r = module_.analyze({{"mission", withEquipment("missing")}});
    EXPECT_EQ(r.status, RFMOD_ERR_HOST_CALL);
    EXPECT_NE(r.body.at("error").at("message").get<std::string>().find("library.get"), std::string::npos);
}

TEST_F(FailsafeModule, TerrainHoleOnTheWayHomeIsReported) {
    terrain_ = [](const LatLon& p) { return p.lon > 35.21 && p.lon < 35.215 ? std::nullopt : std::optional<double>(100.0); };
    const auto r = module_.analyze({{"mission", eastMission(world_, 3.0, 50.0, "control", true)}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_NE(r.body.at("reasons").back().at("text").get<std::string>().find("Failsafe not checked"), std::string::npos);
}
