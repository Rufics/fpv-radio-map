#include "analysis.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace fpv;
using namespace fpv::minalt;

namespace {

// Independent flat-earth check: with k huge the effective earth is flat, so clearance can be
// computed from plain plane geometry here and compared with rf-core's exact construction.
double flatClearance(const std::vector<GroundSample>& g, double hp, double hd, double lambda) {
    const double D = g.back().distanceM;
    const double y0 = g.front().groundMslM + hp, y1 = g.back().groundMslM + hd;
    const double len = std::hypot(D, y1 - y0);
    const double ux = D / len, uy = (y1 - y0) / len;
    double worst = 1e300;
    for (std::size_t i = 1; i + 1 < g.size(); ++i) {
        const double px = g[i].distanceM, py = g[i].groundMslM + g[i].obstacleM - y0;
        const double d1 = px * ux + py * uy;
        const double above = -px * uy + py * ux;  // signed distance above the ray
        const double d2 = len - d1;
        if (d1 <= 0 || d2 <= 0) continue;
        const double r1 = std::sqrt(lambda * d1 * d2 / (d1 + d2));
        worst = std::min(worst, -above / r1);
    }
    return worst;
}

LocalWorld wallWorld(double wallTop) {
    return LocalWorld(
        [wallTop](const LatLon& p) {
            const double d = (p.lon - 35.2) * 73800.0;
            return std::optional<double>(d > 590.0 && d < 610.0 ? wallTop : 100.0);
        },
        5.0);
}

}  // namespace

TEST(MinAltitude, MatchesFlatGeometryWhenCurvatureVanishes) {
    const auto w = wallWorld(140.0);
    const LatLon a{48.5, 35.2};
    const LatLon b = w.direct(a, 90.0, 1500.0).point;
    const auto g = w.groundProfile(a, b);
    const auto inv = w.inverse(a, b);
    const auto mid = w.direct(a, inv.azimuth1Deg, inv.distanceM / 2.0);
    const auto profile = ClearanceSolver::toProfile(g, mid.azimuthDeg, mid.point.lat);
    const double lambda = rfcore::wavelength(rfcore::Hertz(2.44e9)).value();
    const ClearanceSolver solver(1e9, rfcore::Hertz(2.44e9), 0.6);
    for (double h : {10.0, 60.0, 120.0}) {
        EXPECT_NEAR(solver.clearance(profile, 1.5, h), flatClearance(g, 1.5, h, lambda), 1e-6) << h;
    }
    const auto need = solver.requiredAgl(profile, 1.5, 500.0);
    ASSERT_TRUE(need);
    EXPECT_GE(flatClearance(g, 1.5, *need, lambda), 0.6 - 1e-9);
    EXPECT_LT(flatClearance(g, 1.5, *need - 2e-3, lambda), 0.6);
}

TEST(MinAltitude, NothingWhenNoHeightWorks) {
    const auto w = wallWorld(2000.0);
    const LatLon a{48.5, 35.2};
    const LatLon b = w.direct(a, 90.0, 1500.0).point;
    const auto inv = w.inverse(a, b);
    const auto mid = w.direct(a, inv.azimuth1Deg, inv.distanceM / 2.0);
    const auto profile = ClearanceSolver::toProfile(w.groundProfile(a, b), mid.azimuthDeg, mid.point.lat);
    EXPECT_FALSE(ClearanceSolver(4.0 / 3.0, rfcore::Hertz(5.8e9), 0.6).requiredAgl(profile, 1.5, 300.0).has_value());
}

TEST(MinAltitude, RouteAnalysisSuggestsRaisingTheLeg) {
    const auto w = wallWorld(140.0);
    const LatLon a{48.5, 35.2};
    const LatLon b = w.direct(a, 90.0, 1500.0).point;
    const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", 20.0}, {"label", "Target"}}}},
                          {"returnToPilot", false},
                          {"evaluate", "control"}};
    const json r = analyze({{"mission", mission}}, w, {{"control", {rfcore::Hertz(2.44e9), "test radio"}}});
    ASSERT_EQ(r.at("reasons").size(), 1u);
    const json& reasonJ = r.at("reasons").at(0);
    EXPECT_EQ(reasonJ.at("severity"), "warn");
    const double raised = reasonJ.at("fix").at("patch").at("waypoints").at(0).at("aglM").get<double>();
    EXPECT_GT(raised, 20.0);
    // Every checked point on the raised route is clear: rerun with the patch applied.
    json fixed = mission;
    fixed["waypoints"] = reasonJ.at("fix").at("patch").at("waypoints");
    const json again = analyze({{"mission", fixed}}, w, {{"control", {rfcore::Hertz(2.44e9), "test radio"}}});
    EXPECT_TRUE(again.at("reasons").empty()) << again.dump(2);
}
