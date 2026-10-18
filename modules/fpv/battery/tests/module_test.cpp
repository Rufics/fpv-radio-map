// fpv.battery entry: route.analyze through the host ABI, library and weather lookups.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class BatteryModule : public ModuleEntryTest {
protected:
    void SetUp() override {
        ModuleEntryTest::SetUp();
        host_.libraryItem("drones", "quad", {{"id", "quad"}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0},
                                             {"cruisePowerW", 100.0}, {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}});
        host_.libraryItem("batteries", "pack", {{"id", "pack"}, {"cellsSeries", 6}, {"capacityAh", 1.5},
                                                {"nominalCellVoltage", 3.7}, {"usableFraction", 0.8}});
    }

    [[nodiscard]] json mission() const {
        json m = eastMission(world_, 2.0, 0.0, "control", true);
        m["drone"] = "quad";
        m["battery"] = "pack";
        return m;
    }
};

}  // namespace

TEST_F(BatteryModule, StandardEntry) { expectStandardEntry("fpv.battery"); }

TEST_F(BatteryModule, AsksForEquipmentFirst) {
    const auto r = module_.analyze({{"mission", eastMission(world_, 2.0, 0.0)}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "info");
    EXPECT_EQ(host_.calls("library.get"), 0);
}

TEST_F(BatteryModule, FetchesTheForecastWhenTheRequestHasNone) {
    host_.on("weather.point", [](const json& r) {
        EXPECT_EQ(r.at("time"), "2026-10-18T12:00:00Z");
        return json{{"windSpeedMps", 5.0}, {"windFromDeg", 270.0}};
    });
    const auto calm = module_.analyze({{"mission", mission()}, {"weather", {{"windSpeedMps", 0.0}}}});
    const auto windy = module_.analyze({{"mission", mission()}, {"time", "2026-10-18T12:00:00Z"}});
    ASSERT_EQ(calm.status, RFMOD_OK) << calm.body.dump();
    ASSERT_EQ(windy.status, RFMOD_OK) << windy.body.dump();
    EXPECT_EQ(host_.calls("weather.point"), 1);
    // Out with a 5 m/s tailwind, back into it: more time in the air than in calm air.
    EXPECT_GT(windy.body.at("data").at("neededWh").get<double>(), calm.body.at("data").at("neededWh").get<double>());
}

TEST_F(BatteryModule, MissingForecastFallsBackToNoWeather) {
    failHostCall("weather.point");
    const auto r = module_.analyze({{"mission", mission()}, {"weather", nullptr}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(host_.calls("weather.point"), 1);
    EXPECT_GT(r.body.at("data").at("neededWh").get<double>(), 0.0);
}
