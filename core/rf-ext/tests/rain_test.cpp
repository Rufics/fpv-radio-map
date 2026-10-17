#include "rfext/rain.hpp"

#include <rfcore/units.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace rfext;
using rfcore::Hertz;

TEST(Rain, CoefficientsMatchP838Table) {
    // ITU-R P.838-3 Table 5 (values given to the published digits).
    struct Row {
        double ghz, kH, aH, kV, aV;
    };
    for (const Row& r : {Row{1, 0.0000259, 0.9691, 0.0000308, 0.8592}, Row{6, 0.0007056, 1.5900, 0.0004878, 1.5728},
                         Row{10, 0.01217, 1.2571, 0.01129, 1.2156}}) {
        const auto c = RainAttenuation::coefficients(Hertz(r.ghz * 1e9));
        EXPECT_NEAR(c.kH / r.kH, 1.0, 5e-4) << r.ghz;
        EXPECT_NEAR(c.alphaH, r.aH, 5e-5) << r.ghz;
        EXPECT_NEAR(c.kV / r.kV, 1.0, 5e-4) << r.ghz;
        EXPECT_NEAR(c.alphaV, r.aV, 5e-5) << r.ghz;
    }
    EXPECT_THROW((void)RainAttenuation::coefficients(Hertz(0.9e9)), std::invalid_argument);
}

TEST(Rain, PolarisationCombination) {
    const Hertz f(5.8e9);
    const auto c = RainAttenuation::coefficients(f);
    // Horizontal path, horizontal polarisation: plain kH R^αH.
    EXPECT_NEAR(RainAttenuation::specificDbPerKm(f, 25.0, 0.0, 0.0), c.kH * std::pow(25.0, c.alphaH), 1e-15);
    EXPECT_NEAR(RainAttenuation::specificDbPerKm(f, 25.0, 0.0, 90.0), c.kV * std::pow(25.0, c.alphaV), 1e-15);
    // Circular: k is the mean of kH and kV.
    const double k = (c.kH + c.kV) / 2.0;
    const double a = (c.kH * c.alphaH + c.kV * c.alphaV) / (2.0 * k);
    EXPECT_NEAR(RainAttenuation::specificDbPerKm(f, 25.0, 3.0, 45.0), k * std::pow(25.0, a), 1e-15);
    EXPECT_EQ(RainAttenuation::specificDbPerKm(f, 0.0, 0.0, 45.0), 0.0);
}
