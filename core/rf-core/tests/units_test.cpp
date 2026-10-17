#include "rfcore/units.hpp"

#include <gtest/gtest.h>
#include <type_traits>

using namespace rfcore;
using namespace rfcore::literals;

TEST(Units, LevelPlusRatioIsLevel) {
    const DecibelMilliwatt p = 20_dBm + 3_dB;
    EXPECT_DOUBLE_EQ(p.value(), 23.0);
    const Decibel diff = 23_dBm - 20_dBm;
    EXPECT_DOUBLE_EQ(diff.value(), 3.0);
}

TEST(Units, MixingUnitsDoesNotCompile) {
    EXPECT_FALSE((std::is_invocable_v<std::plus<>, Metres, Hertz>));
    EXPECT_FALSE((std::is_invocable_v<std::plus<>, DecibelMilliwatt, DecibelMilliwatt>));
    EXPECT_FALSE((std::is_convertible_v<double, Metres>));
}

TEST(Units, WattsRoundTrip) {
    EXPECT_DOUBLE_EQ(DecibelMilliwatt::fromWatts(Watt(1.0)).value(), 30.0);
    EXPECT_DOUBLE_EQ((30_dBm).toWatts().value(), 1.0);
}
