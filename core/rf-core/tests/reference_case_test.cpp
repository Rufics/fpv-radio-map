// The reference case from the design (leg 3, A to drone at B). These are the numbers the
// mock-ups show; if the maths changes, this test says which number moved.

#include "rfcore/link_budget.hpp"
#include "rfcore/propagation.hpp"
#include "test_profiles.hpp"

#include <gtest/gtest.h>

using namespace rfcore;
using namespace rfcore::literals;

namespace {
const Metres kD1(2583.5417550331947364);

LinkResult run(Hertz f, double ridgeTop) {
    // Horizontal ray at 546 m; the ridge stands ridgeTop - 546 above it.
    const auto profile = test::spikeProfile(5280.0, kD1.value(), 0.0, ridgeTop);
    const LinkEvaluator eval(EarthModel(1e12), BullingtonDiffraction());
    TransmitterEnd tx;
    tx.antennaAgl = 546_m; tx.antennaGain = DecibelIsotropic(4.0); tx.feederLoss = 0_dB; tx.power = 30_dBm;
    ReceiverEnd rx;
    rx.antennaAgl = 546_m; rx.antennaGain = DecibelIsotropic(3.0); rx.feederLoss = 0_dB;
    rx.sensitivity = DecibelMilliwatt(-105.0); rx.requiredMargin = 10_dB;
    return eval.evaluate(profile, f, tx, rx);
}
}  // namespace

TEST(ReferenceCase, Video915Blocked) {
    const LinkResult r = run(915_MHz, 646.0);
    EXPECT_NEAR(r.diffraction.loss.value(), 29.606874833812861, 1e-6);   // shown 29.61 dB
    EXPECT_NEAR(r.received.value(), -98.73575838770162, 1e-6);           // shown −98.74 dBm
    EXPECT_NEAR(r.margin.value(), -3.7357583877016, 1e-6);               // shown −3.74 dB
    EXPECT_FALSE(r.reachable());
}

TEST(ReferenceCase, Control433Reachable) {
    const LinkResult r = run(433_MHz, 646.0);
    EXPECT_NEAR(r.diffraction.loss.value(), 26.361073136555449564, 1e-6);  // shown 26.36 dB
    EXPECT_NEAR(r.margin.value(), 37.0 - 99.63021959962692784 - 26.361073136555449564 + 95.0, 1e-6);  // +6.01
    EXPECT_TRUE(r.reachable());
}

TEST(ReferenceCase, FixRaisesClearance) {
    // The fix leaves the ray 10.094108879255458 m above the ridge (ν = −0.6866, J = 0.55 dB).
    const LinkResult r = run(915_MHz, 546.0 - 10.094108879255458);
    EXPECT_TRUE(r.diffraction.lineOfSight);
    EXPECT_NEAR(r.diffraction.loss.value(), 0.55090552676348360945, 1e-6);
    EXPECT_NEAR(r.margin.value(), 25.320210919347898, 1e-5);  // shown +25.32 dB
}
