#include "rfcore/link_budget.hpp"
#include "test_profiles.hpp"

#include <gtest/gtest.h>

using namespace rfcore;
using namespace rfcore::literals;

namespace {
class FixedLoss : public ILossContributor {
public:
    LossContribution contribute(const PathContext&) const override {
        return {{{"test/forest", "Forest", 7_dB, "test"}}, {{"Forest height", "assumed for the test"}}};
    }
};
}  // namespace

TEST(LinkEvaluator, ContributorsAddNamedTerms) {
    const auto profile = test::spikeProfile(5280.0, 2583.5417550331947364, 0.0, 646.0);
    LinkEvaluator eval(EarthModel(1e12), BullingtonDiffraction());
    eval.addContributor(std::make_shared<FixedLoss>());
    TransmitterEnd tx;
    tx.antennaAgl = 546_m; tx.antennaGain = DecibelIsotropic(4.0); tx.feederLoss = 0_dB; tx.power = 30_dBm;
    ReceiverEnd rx;
    rx.antennaAgl = 546_m; rx.antennaGain = DecibelIsotropic(3.0); rx.feederLoss = 0_dB;
    rx.sensitivity = DecibelMilliwatt(-105.0); rx.requiredMargin = 10_dB;
    const LinkResult r = eval.evaluate(profile, 915_MHz, tx, rx);

    bool found = false;
    for (const auto& t : r.terms) found = found || t.id == "test/forest";
    EXPECT_TRUE(found);
    EXPECT_NEAR(r.margin.value(), -3.7357583877016 - 7.0, 1e-6);
    ASSERT_EQ(r.uncertainty.size(), 2u);
    EXPECT_EQ(r.uncertainty[0].what, "Terrain data");
}
