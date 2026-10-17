#include "rfcore/fresnel_integrals.hpp"
#include "rfcore/propagation.hpp"

#include <gtest/gtest.h>

#include <cmath>

using rfcore::FresnelIntegrals;
using rfcore::KnifeEdge;

namespace {
// Reference values from mpmath at 40 significant digits (fresnelc, fresnels).
struct Ref { double x; double c; double s; double j; };
constexpr Ref kRefs[] = {
    {0, 0.0, 0.0, 6.0205999132796239043},
    {1e-08, 1.0000000000000000209e-8, 5.2359877559829890594e-25, 6.0206000001385202849},
    {0.1, 0.099997532627085073601, 0.00052358954761221068668, 6.887987238434577441},
    {0.5, 0.49234422587144639288, 0.064732432859999277611, 10.233830466326909221},
    {1, 0.77989340037682282947, 0.43825914739035476608, 13.864105413629098008},
    {1.4, 0.54309578354625647721, 0.71352507736341210738, 16.247919005695017295},
    {1.5, 0.44526117603982153506, 0.69750496008209301308, 16.777336788323994281},
    {1.6, 0.36546168344048765296, 0.63888768350938083462, 17.282704970923115848},
    {2, 0.4882534060753407545, 0.3434156783636982422, 19.090962378661638964},
    {3, 0.60572078929768562956, 0.4963129989673750361, 22.52181308754068863},
    {4.679085311440794, 0.51225683105295471064, 0.56687867520420374279, 26.361073136555449564},
    {6.801858020653234, 0.48135631178056136031, 0.5429173694698706958, 29.606874833812861091},
    {10, 0.49989869420551572361, 0.4681699785848822404, 32.95351734806841608},
    {50, 0.49999918943072796796, 0.49363380258593874145, 46.93269784926851133},
    {1000, 0.49999999989867881636, 0.49968169011381630608, 72.953297410524689201},
    {-0.6865869544171077, -0.64988469064413768122, -0.16294462507854126296, 0.55090552676348360945},
    {-3, -0.60572078929768562956, -0.4963129989673750361, -0.4439432888179673334},
};
// A few units in the last place of double.
constexpr double kRel = 1e-14;
}  // namespace

TEST(FresnelIntegrals, MatchesHighPrecisionReference) {
    for (const Ref& r : kRefs) {
        const auto v = FresnelIntegrals::evaluate(r.x);
        EXPECT_NEAR(v.c, r.c, std::abs(r.c) * kRel + 1e-300) << "C(" << r.x << ")";
        EXPECT_NEAR(v.s, r.s, std::abs(r.s) * kRel + 1e-300) << "S(" << r.x << ")";
    }
}

TEST(KnifeEdge, ExactLossMatchesReference) {
    for (const Ref& r : kRefs) {
        EXPECT_NEAR(KnifeEdge::loss(r.x).value(), r.j, std::abs(r.j) * kRel + 1e-13) << "J(" << r.x << ")";
    }
}

TEST(KnifeEdge, GrazingIsSixDecibels) {
    EXPECT_NEAR(KnifeEdge::loss(0.0).value(), 20.0 * std::log10(2.0), 1e-14);
}
