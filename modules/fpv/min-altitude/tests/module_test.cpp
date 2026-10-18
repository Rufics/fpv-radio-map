// fpv.min-altitude entry: route.analyze through the host ABI and radio profile lookups.

#include <fake_host.hpp>

#include <gtest/gtest.h>

using namespace fpv;
using namespace fpv::test;

namespace {

class MinAltitudeModule : public ModuleEntryTest {
protected:
    void SetUp() override {
        ModuleEntryTest::SetUp();
        host_.libraryItem("radio-profiles", "elrs-900", {{"id", "elrs-900"}, {"name", "ELRS 900"}, {"frequencyHz", 915e6}});
        // A 40 m wall half way to the target, 1 km east.
        terrain_ = [](const LatLon& p) {
            const double d = (p.lon - 35.2) * 73800.0;
            return std::optional<double>(d > 480.0 && d < 520.0 ? 140.0 : 100.0);
        };
    }
};

}  // namespace

TEST_F(MinAltitudeModule, StandardEntry) { expectStandardEntry("fpv.min-altitude"); }

TEST_F(MinAltitudeModule, LooksUpEachConfiguredLinkRadio) {
    json m = eastMission(world_, 1.0, 5.0, "both");
    // Only the control link is set up; the video link without a setup is skipped.
    m["links"] = {{"control", {{"radio", "elrs-900"}}}, {"video", "none"}};
    const auto r = module_.analyze({{"mission", m}, {"options", {{"stepM", 250.0}}}});
    ASSERT_EQ(r.status, RFMOD_OK) << r.body.dump();
    EXPECT_EQ(host_.calls("library.get"), 1);
    EXPECT_TRUE(r.body.at("data").at("links").contains("control"));
    EXPECT_FALSE(r.body.at("data").at("links").contains("video"));
    EXPECT_GT(host_.calls("profile.get"), 0);
    ASSERT_FALSE(r.body.at("reasons").empty());
    EXPECT_EQ(r.body.at("reasons").at(0).at("severity"), "warn");
}

TEST_F(MinAltitudeModule, UnknownRadioIsAHostError) {
    json m = eastMission(world_, 1.0, 5.0);
    m["links"] = {{"control", {{"radio", "nope"}}}};
    const auto r = module_.analyze({{"mission", m}});
    EXPECT_EQ(r.status, RFMOD_ERR_HOST_CALL);
}
