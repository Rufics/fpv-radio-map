#include "rfcore/diffraction.hpp"
#include "rfcore/propagation.hpp"
#include "test_profiles.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace rfcore;
using namespace rfcore::literals;

namespace {
// k so large that the earth is flat to far below a nanometre over 10 km.
const EarthModel kFlat(1e12);
const Metres kD1(2583.5417550331947364);
}  // namespace

TEST(Bullington, SingleEdgeOnFlatEarthEqualsKnifeEdge) {
    // Both antennas at 546 m MSL, so the ray is horizontal and the edge is exactly 100 m above it.
    const auto profile = test::spikeProfile(5280.0, kD1.value(), 0.0, 646.0);
    const PathGeometry g(profile, kFlat, 546_m, 546_m);
    const auto r = BullingtonDiffraction().compute(g, wavelength(915_MHz));
    EXPECT_FALSE(r.lineOfSight);
    EXPECT_NEAR(r.edgeHeight.value(), 100.0, 1e-6);
    EXPECT_NEAR(r.d1.value(), kD1.value(), 1e-6);
    EXPECT_NEAR(r.nu, 6.8018580206532337599, 1e-7);
    EXPECT_NEAR(r.loss.value(), 29.606874833812861091, 1e-6);
    EXPECT_LT(r.minFresnelClearance, 0.0);
}

TEST(Bullington, ClearPathUsesLargestNu) {
    const auto profile = test::spikeProfile(5280.0, kD1.value(), 0.0, 400.0);
    const PathGeometry g(profile, kFlat, 546_m, 546_m);
    const auto r = BullingtonDiffraction().compute(g, wavelength(915_MHz));
    EXPECT_TRUE(r.lineOfSight);
    EXPECT_NEAR(r.edgeHeight.value(), -146.0, 1e-6);
    EXPECT_NEAR(r.minFresnelClearance, 146.0 / 20.791577214328232144, 1e-6);
}

TEST(Bullington, TwoRidgesIntersectTangents) {
    // Ridges at 1/3 and 2/3 of a 6 km path, antennas at 100 m. Flat earth, so the tangent
    // lines from each antenna meet in the middle at a height we can compute by hand.
    const auto profile = test::makeProfile(
        {{0, 0}, {1000, 0}, {2000, 200}, {3000, 0}, {4000, 250}, {5000, 0}, {6000, 0}});
    const PathGeometry g(profile, kFlat, 100_m, 100_m);
    const auto r = BullingtonDiffraction().compute(g, wavelength(915_MHz));
    // From A: slope (200-100)/2000 = 0.05; from B: slope (250-100)/2000 = 0.075.
    // Meeting point x: 100 + 0.05 x = 100 + 0.075 (6000 - x) -> x = 3600, height 280.
    EXPECT_FALSE(r.lineOfSight);
    EXPECT_NEAR(r.d1.value(), 3600.0, 1e-6);
    EXPECT_NEAR(r.edgeHeight.value(), 180.0, 1e-6);
}

TEST(PathGeometry, CurvedEarthMatchesBulgeFormulaClosely) {
    // On a curved earth the exact geometry and the textbook bulge d1 d2 / (2 k R) differ
    // by a negligible amount; this guards the circle construction.
    const EarthModel std43(4.0 / 3.0);
    const auto profile = test::spikeProfile(20000.0, 10000.0, 0.0, 0.0, 100.0);
    const PathGeometry g(profile, std43, 10_m, 10_m);
    const double a = g.effectiveRadius().value();
    const auto rel = g.relate(g.tops()[100]);  // ground at the midpoint
    const double bulge = 10000.0 * 10000.0 / (2.0 * a);
    EXPECT_NEAR(rel.heightAbove.value(), bulge - 10.0, 1e-3);
}

TEST(Bullington, EmpiricalCorrectionFormula) {
    const Decibel c = BullingtonDiffraction::empiricalCorrection(Decibel(29.606874833812861), 5280_m);
    EXPECT_NEAR(c.value(), (1.0 - std::exp(-29.606874833812861 / 6.0)) * (10.0 + 0.1056), 1e-12);
}
