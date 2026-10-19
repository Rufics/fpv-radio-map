#include "rfmod/semver.hpp"

#include <gtest/gtest.h>

using rfmod::Version;
using rfmod::VersionRange;

TEST(Semver, ParsesAndOrders) {
    EXPECT_EQ(Version::parse("1.2.3").str(), "1.2.3");
    EXPECT_EQ(Version::parse("1.2.3-beta.2+build.7").str(), "1.2.3-beta.2");
    // SemVer §11 example ordering.
    const char* ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                             "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(ordered); ++i) {
        EXPECT_LT(Version::parse(ordered[i]), Version::parse(ordered[i + 1])) << ordered[i];
    }
    EXPECT_LT(Version::parse("1.9.0"), Version::parse("1.10.0"));
}

TEST(Semver, RejectsInvalid) {
    for (const char* bad : {"1.2", "01.2.3", "1.2.3-", "1.2.3-01", "a.b.c", "1.2.3.4.5", "1.2.99999999999"}) {
        EXPECT_THROW((void)Version::parse(bad), std::invalid_argument) << bad;
    }
}

TEST(Semver, Ranges) {
    const auto r = VersionRange::parse(">=1.2.0 <2.0.0");
    EXPECT_TRUE(r.contains(Version::parse("1.2.0")));
    EXPECT_TRUE(r.contains(Version::parse("1.99.0")));
    EXPECT_FALSE(r.contains(Version::parse("2.0.0")));
    EXPECT_FALSE(r.contains(Version::parse("1.5.0-beta")));  // prerelease needs an explicit opt-in

    EXPECT_TRUE(VersionRange::parse("^0.2.3").contains(Version::parse("0.2.9")));
    EXPECT_FALSE(VersionRange::parse("^0.2.3").contains(Version::parse("0.3.0")));
    EXPECT_FALSE(VersionRange::parse("~1.4.2").contains(Version::parse("1.5.0")));
    EXPECT_TRUE(VersionRange::parse("<1.0.0 || >=3.0.0").contains(Version::parse("3.1.0")));
    EXPECT_TRUE(VersionRange::parse("*").contains(Version::parse("9.9.9")));
    EXPECT_TRUE(VersionRange::parse(">=1.0.0-rc.1").contains(Version::parse("1.0.0-rc.2")));
    EXPECT_THROW((void)VersionRange::parse(">=1.0 <2"), std::invalid_argument);
}

TEST(Semver, CaretAndTildeAtTheLargestNumber) {
    // The upper bound would wrap around to 0 and exclude the version itself (found by fuzzing).
    EXPECT_THROW((void)VersionRange::parse("^4294967295.0.0"), std::invalid_argument);
    EXPECT_THROW((void)VersionRange::parse("^0.0.4294967295"), std::invalid_argument);
    EXPECT_THROW((void)VersionRange::parse("~1.4294967295.0"), std::invalid_argument);
    EXPECT_TRUE(VersionRange::parse("^4294967294.0.0").contains(Version::parse("4294967294.7.0")));
}

TEST(Semver, EveryComparatorOperator) {
    EXPECT_TRUE(VersionRange::parse("=1.2.3").contains(Version::parse("1.2.3")));
    EXPECT_FALSE(VersionRange::parse("=1.2.3").contains(Version::parse("1.2.4")));
    EXPECT_TRUE(VersionRange::parse(">1.0.0").contains(Version::parse("1.0.1")));
    EXPECT_FALSE(VersionRange::parse(">1.0.0").contains(Version::parse("1.0.0")));
    EXPECT_TRUE(VersionRange::parse("<=2.0.0").contains(Version::parse("2.0.0")));
    EXPECT_FALSE(VersionRange::parse("<=2.0.0").contains(Version::parse("2.0.1")));
}
