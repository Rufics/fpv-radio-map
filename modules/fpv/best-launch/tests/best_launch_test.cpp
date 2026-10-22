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
