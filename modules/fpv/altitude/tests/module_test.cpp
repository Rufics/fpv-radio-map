// fpv.altitude entry: route.analyze through the host ABI and radio profile lookups.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class AltitudeModule : public ModuleEntryTest {
protected:
    void SetUp() override {
        ModuleEntryTest::SetUp();
        host_.libraryItem("radio-profiles", "elrs-900", {{"id", "elrs-900"}, {"name", "ELRS 900"}, {"frequencyHz", 915e6}});
    }
};

}  // namespace

TEST_F(AltitudeModule, StandardEntry) { expectStandardEntry("fpv.altitude"); }

TEST_F(AltitudeModule, DrawsTheCorridorWithTheConfiguredLink) {
    json m = eastMission(world_, 1.0, 60.0, "both");
    m["links"] = {{"control", {{"radio", "elrs-900"}}}, {"video", "none"}};
    const auto r = module_.analyze({{"mission", m}, {"options", {{"stepM", 250.0}}}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(host_.calls("library.get"), 1);
    const json& data = r.body.at("data");
    EXPECT_EQ(data.at("fresnel").at("radio"), "ELRS 900");
    EXPECT_EQ(data.at("corridor").at("s").size(), 5U);
    EXPECT_EQ(data.at("reference"), "agl");
    EXPECT_DOUBLE_EQ(data.at("pilot").at("antennaMslM").get<double>(), 101.5);
}

TEST_F(AltitudeModule, UnknownRadioIsAHostError) {
    json m = eastMission(world_, 1.0, 60.0);
    m["links"] = {{"control", {{"radio", "nope"}}}};
    EXPECT_EQ(module_.analyze({{"mission", m}}).status, RFMOD_ERR_HOST_CALL);
}

TEST_F(AltitudeModule, ARadioWithoutAFrequencyIsABadRequest) {
    host_.libraryItem("radio-profiles", "broken", {{"id", "broken"}, {"frequencyHz", 0.0}});
    json m = eastMission(world_, 1.0, 60.0);
    m["links"] = {{"control", {{"radio", "broken"}}}};
    const auto r = module_.analyze({{"mission", m}});
    EXPECT_EQ(r.status, RFMOD_ERR_BAD_REQUEST);
    EXPECT_NE(r.body.dump().find("frequencyHz"), std::string::npos);
}
