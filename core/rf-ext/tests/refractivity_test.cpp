#include "rfext/refractivity.hpp"

#include <rfcore/earth_model.hpp>

#include <gtest/gtest.h>

using namespace rfext;

TEST(Refractivity, MatchesP453Formulae) {
    // Reference computed independently from P.453-14 eqs. (2)-(4), (9)-(10) in double precision.
    EXPECT_NEAR(Refractivity::saturationVapourPressure(20.0, 1013.25), 23.48164577004656, 1e-12);
    EXPECT_NEAR(Refractivity::n({rfcore::Metres(0.0), 20.0, 1013.25, 60.0}), 329.428809635152, 1e-10);
    // Dry air: only the first term.
    EXPECT_NEAR(Refractivity::n({rfcore::Metres(0.0), 15.0, 1013.25, 0.0}), 77.6 * 1013.25 / 288.15, 1e-12);
    EXPECT_THROW((void)Refractivity::n({rfcore::Metres(0.0), 15.0, 1013.25, 120.0}), std::invalid_argument);
}

TEST(Refractivity, PicksTheLayerAboveTheHigherAntenna) {
    const std::vector<AirSample> column = {
        {rfcore::Metres(1500.0), 5.0, 850.0, 50.0},
        {rfcore::Metres(150.0), 18.0, 1000.0, 70.0},  // surface, given out of order
        {rfcore::Metres(800.0), 12.0, 925.0, 60.0},
    };
    const auto r = refractionForPath(column, rfcore::Metres(420.0));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->layerBottom.value(), 150.0);
    EXPECT_EQ(r->layerTop.value(), 800.0);
    EXPECT_DOUBLE_EQ(r->gradientPerKm, Refractivity::gradientPerKm(column[1], column[2]));
    EXPECT_FALSE(refractionForPath(column, rfcore::Metres(2000.0)));
    // A typical gradient gives k close to 4/3.
    const double k = rfcore::EarthModel::factorFromRefractivityGradient(-40.0, rfcore::Metres(6371000.0));
    EXPECT_NEAR(k, 1.0 / (1.0 - 6371.0 * 40.0e-6), 1e-12);
}
