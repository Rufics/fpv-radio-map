#include "rfcore/propagation.hpp"
#include "rfcore/receiver.hpp"

#include <gtest/gtest.h>

using namespace rfcore;
using namespace rfcore::literals;

TEST(FreeSpaceLoss, ReferenceValues) {
    EXPECT_NEAR(FreeSpaceLoss::compute(5280_m, 915_MHz).value(), 106.12888355388858451, 1e-12);
    EXPECT_NEAR(FreeSpaceLoss::compute(5280_m, 433_MHz).value(), 99.63021959962692784, 1e-12);
}

TEST(FresnelZone, FirstZoneRadius) {
    const Metres d1(2583.5417550331947364);
    const Metres d2 = 5280_m - d1;
    EXPECT_NEAR(FresnelZone::radius(1, wavelength(915_MHz), d1, d2).value(), 20.791577214328232144, 1e-12);
    EXPECT_NEAR(FresnelZone::radius(1, wavelength(433_MHz), d1, d2).value(), 30.224145708889146295, 1e-12);
}

TEST(KnifeEdge, NuReference) {
    const Metres d1(2583.5417550331947364);
    const Metres d2 = 5280_m - d1;
    EXPECT_NEAR(KnifeEdge::nu(100_m, wavelength(915_MHz), d1, d2), 6.8018580206532337599, 1e-13);
}

TEST(ReceiverSensitivity, ThermalNoiseFloor) {
    EXPECT_NEAR(ReceiverSensitivity::thermalNoiseFloor(Hertz(1.0)).value(), -173.97518719422810442, 1e-12);
    EXPECT_NEAR(ReceiverSensitivity::thermalNoiseFloor(Hertz(500e3)).value(), -116.98548715086791637, 1e-12);
    EXPECT_NEAR(ReceiverSensitivity::compute(Hertz(20e6), 5_dB, 10_dB).value(), -100.96488723758829246 + 15.0, 1e-12);
}
