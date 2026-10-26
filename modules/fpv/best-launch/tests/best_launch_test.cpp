#include "best_launch.hpp"

#include <gtest/gtest.h>

using namespace fpv;

namespace {

class DistanceLink final : public ILinkEvaluator {
public:
    explicit DistanceLink(const IWorld& w) : w_(w) {}
    double marginDb(const std::string& link, const End& g, const End& d) const override {
        ++calls;
        return (link == "video" ? 20.0 : 30.0) - w_.inverse(g.position, d.position).distanceM / 100.0;
    }
    mutable int calls = 0;

private:
    const IWorld& w_;
};

}  // namespace

TEST(Search, DrawnAreaUsesMercatorEdges) {
    const DrawnArea a = DrawnArea::fromJson({{{"lat", 0}, {"lon", 0}}, {{"lat", 60}, {"lon", 0}}, {{"lat", 60}, {"lon", 10}}});
    // The hypotenuse from (0,0) to (60,10) is straight in Mercator: at lon 5 it crosses where
    // mercY = mercY(60)/2, i.e. lat ≈ 36.65°, not lat 30 (the plate carrée midpoint).
    EXPECT_TRUE(a.contains({40.0, 5.0}));
    EXPECT_FALSE(a.contains({33.0, 5.0}));
}

TEST(Search, GridRespectsTheLimit) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(0.0); }, 30.0);
    const DrawnArea a = DrawnArea::fromJson({{{"lat", 48.0}, {"lon", 35.0}}, {{"lat", 48.01}, {"lon", 35.0}},
                                             {{"lat", 48.01}, {"lon", 35.01}}, {{"lat", 48.0}, {"lon", 35.01}}});
    const auto g = a.grid(w, 100.0, 1000);
    EXPECT_GT(g.size(), 50u);  // about 11 x 7 cells of 100 m
    for (const auto& p : g) EXPECT_TRUE(a.contains(p));
    EXPECT_THROW((void)a.grid(w, 10.0, 100), rfmod::BadRequest);
}

TEST(Search, GridCountsTheCellsItVisits) {
    // The bounding-box estimate looks at the box's edges; on a world whose rows widen towards
    // the middle the loop itself has to notice. A degree is a kilometre here, except that the
    // middle rows are a thousand times wider.
    class WidensInTheMiddle final : public IWorld {
    public:
        [[nodiscard]] InverseResult inverse(const LatLon& a, const LatLon& b) const override {
            const double bulge = std::abs(a.lat - 0.5) < 0.25 ? 1000.0 : 1.0;
            return {std::abs(b.lat - a.lat) * 1000.0 + std::abs(b.lon - a.lon) * 1000.0 * bulge, 0.0, 0.0};
        }
        [[nodiscard]] DirectResult direct(const LatLon& a, double azimuthDeg, double distanceM) const override {
            const double deg = distanceM / 1000.0;
            return {azimuthDeg == 0.0 ? LatLon{a.lat + deg, a.lon} : LatLon{a.lat, a.lon + deg}, azimuthDeg};
        }
        [[nodiscard]] double groundMsl(const LatLon&) const override { return 0.0; }
        [[nodiscard]] std::vector<GroundSample> groundProfile(const LatLon&, const LatLon&) const override { return {}; }
    } w;
    const DrawnArea a = DrawnArea::fromJson({{{"lat", 0.0}, {"lon", 0.0}}, {{"lat", 1.0}, {"lon", 0.0}},
                                             {{"lat", 1.0}, {"lon", 1.0}}, {{"lat", 0.0}, {"lon", 1.0}}});
    // 1000 x 1000 cells by the edges, under the limit; the middle rows hold a million each.
    EXPECT_THROW((void)a.grid(w, 1.0, 10'000'000), rfmod::BadRequest);
}

TEST(BestLaunch, PicksTheBestCandidateAndScoresTheWorstLink) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    const LatLon a{48.5, 35.2};
    const LatLon t = w.direct(a, 90.0, 3000.0).point;
    const LatLon e0 = w.direct(a, 90.0, 1000.0).point, e1 = w.direct(a, 90.0, 2000.0).point;
    const json area = {{{"lat", 48.4995}, {"lon", e0.lon}}, {{"lat", 48.5005}, {"lon", e0.lon}},
                       {{"lat", 48.5005}, {"lon", e1.lon}}, {{"lat", 48.4995}, {"lon", e1.lon}}};
    const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
                          {"returnToPilot", false}, {"evaluate", "both"}};
    const DistanceLink link(w);
    const json r = fpv::launch::analyze({{"mission", mission}, {"options", {{"area", area}, {"cellM", 50.0}, {"stepM", 500.0}}}}, w, link);
    const json& d = r.at("data");
    // From the pilot the worst point is the target: video, 20 - 30 = -10 dB.
    EXPECT_NEAR(d.at("current").at("worstMarginDb").get<double>(), -10.0, 1e-6);
    EXPECT_EQ(d.at("current").at("link"), "video");
    // The best candidate is the one with the smallest worst distance to the route samples: near
    // the middle of the route, so its worst sample is either the start or the target.
    const double best = d.at("candidates").at(0).at("worstMarginDb").get<double>();
    EXPECT_GT(best, -10.0);
    for (const json& c : d.at("candidates")) EXPECT_LE(c.at("worstMarginDb").get<double>(), best);
    EXPECT_TRUE(r.at("reasons").at(0).contains("fix"));
    EXPECT_EQ(r.at("reasons").at(0).at("fix").at("patch").at("pilot").at("aglM"), 1.5);
}

TEST(BestLaunch, AsksForAnAreaFirst) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    const DistanceLink link(w);
    const json mission = {{"pilot", {{"lat", 48.5}, {"lon", 35.2}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", 48.5}, {"lon", 35.3}, {"aglM", 50.0}}}}};
    const json r = fpv::launch::analyze({{"mission", mission}}, w, link);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
    EXPECT_EQ(link.calls, 0);
}

TEST(BestLaunch, NeedsALinkToScore) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    const DistanceLink link(w);
    const json mission = {{"pilot", {{"lat", 48.5}, {"lon", 35.2}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", 48.5}, {"lon", 35.3}, {"aglM", 50.0}}}}, {"evaluate", "none"}};
    const json area = {{{"lat", 48.49}, {"lon", 35.21}}, {{"lat", 48.51}, {"lon", 35.21}}, {{"lat", 48.51}, {"lon", 35.22}}};
    const json r = fpv::launch::analyze({{"mission", mission}, {"options", {{"area", area}}}}, w, link);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
    EXPECT_EQ(link.calls, 0);
}
