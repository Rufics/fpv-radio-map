#include "rfext/clutter.hpp"
#include "test_profiles.hpp"

#include <gtest/gtest.h>

using namespace rfext;

namespace {

class FixedRaster final : public IObstacleHeightSource {
public:
    std::optional<rfcore::Metres> height(const rfcore::GeoPoint&) const override { return rfcore::Metres(21.0); }
    std::string name() const override { return "test canopy raster"; }
};

}  // namespace

TEST(Clutter, RaisesEnabledClassesOnly) {
    auto profile = rfcore::test::makeProfile({{0, 100}, {30, 100}, {60, 100}, {90, 100}, {120, 100}, {150, 100}});
    auto& s = profile.mutableSamples();
    s[0].landCover = 10;  // end sample: untouched
    s[1].landCover = 10;
    s[2].landCover = 50;
    s[3].landCover = 30;
    s[4].landCover = std::nullopt;
    s[5].landCover = 10;
    FixedRaster canopy;
    std::map<LandCover, ClassSetting> settings;
    settings[LandCover::Tree] = {true, std::nullopt, "", &canopy};
    settings[LandCover::BuiltUp] = {true, rfcore::Metres(9.0), "municipal survey 2024", nullptr};
    settings[LandCover::Grassland] = {false, rfcore::Metres(1.0), "x", nullptr};
    const auto report = ClutterConditioner(settings).apply(profile);
    EXPECT_EQ(s[0].obstacleHeight.value(), 0.0);
    EXPECT_EQ(s[1].obstacleHeight.value(), 21.0);
    EXPECT_EQ(s[2].obstacleHeight.value(), 9.0);
    EXPECT_EQ(s[3].obstacleHeight.value(), 0.0);
    EXPECT_EQ(s[5].obstacleHeight.value(), 0.0);
    EXPECT_EQ(report.samplesRaised, 2u);
    EXPECT_EQ(report.samplesWithoutCover, 1u);
    ASSERT_FALSE(report.notes().empty());
    EXPECT_NE(report.notes()[0].detail.find("opaque"), std::string::npos);
}

TEST(Clutter, EnabledClassWithoutHeightIsReported) {
    auto profile = rfcore::test::makeProfile({{0, 100}, {30, 100}, {60, 100}});
    profile.mutableSamples()[1].landCover = 10;
    std::map<LandCover, ClassSetting> settings;
    settings[LandCover::Tree] = {true, std::nullopt, "", nullptr};
    const auto report = ClutterConditioner(settings).apply(profile);
    EXPECT_EQ(report.samplesRaised, 0u);
    EXPECT_EQ(report.samplesWithoutHeight, 1u);
}
