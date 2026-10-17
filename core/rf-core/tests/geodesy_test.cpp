#include "rfcore/earth_model.hpp"
#include "rfcore/geodesy.hpp"

#include <gtest/gtest.h>

using namespace rfcore;

TEST(Geodesy, JfkToLhr) {
    // GeographicLib documentation example: JFK to LHR is 5551.76 km.
    const Geodesy g;
    const auto r = g.inverse({40.6, -73.8}, {51.6, -0.5});
    EXPECT_NEAR(r.distance.value() / 1000.0, 5551.76, 0.005);
}

TEST(Geodesy, DirectInverseRoundTrip) {
    const Geodesy g;
    const GeoPoint a{48.53, 35.20};
    const GeoPoint b = g.direct(a, 37.5, Metres(5280.0));
    const auto r = g.inverse(a, b);
    EXPECT_NEAR(r.distance.value(), 5280.0, 1e-8);
    EXPECT_NEAR(r.azimuthStartDeg, 37.5, 1e-11);
}

TEST(Geodesy, SampleSpacing) {
    const Geodesy g;
    const auto pts = g.sample({48.5, 35.2}, {48.55, 35.25}, Metres(30.0));
    const double total = g.inverse(pts.front(), pts.back()).distance.value();
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double step = g.inverse(pts[i - 1], pts[i]).distance.value();
        EXPECT_LE(step, 30.0 + 1e-9);
        EXPECT_NEAR(step, total / static_cast<double>(pts.size() - 1), 1e-6);
    }
}

TEST(EarthModel, RadiusOfCurvatureLimits) {
    // On the equator facing north the radius is the meridian radius a(1-e²).
    const double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    EXPECT_NEAR(EarthModel::radiusOfCurvature(0.0, 0.0).value(), a * (1 - e2), 1e-6);
    EXPECT_NEAR(EarthModel::radiusOfCurvature(0.0, 90.0).value(), a, 1e-6);
}

TEST(EarthModel, StandardGradientGivesAboutFourThirds) {
    const double k = EarthModel::factorFromRefractivityGradient(-40.0, Metres(6371000.0));
    EXPECT_NEAR(k, 1.0 / (1.0 - 6371000.0 * 40e-9), 1e-15);
    EXPECT_THROW(EarthModel(0.0), std::invalid_argument);
}
