#include "relay.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace fpv;

namespace {

class DistanceLink final : public ILinkEvaluator {
public:
    explicit DistanceLink(const IWorld& w) : w_(w) {}
    double marginDb(const std::string&, const End& g, const End& d) const override {
        return 10.0 - w_.inverse(g.position, d.position).distanceM / 100.0;
    }

private:
    const IWorld& w_;
};

}  // namespace

TEST(Relay, SearchFindsTheMiddleAndSuggestsIt) {
    const LocalWorld w([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    const LatLon a{48.5, 35.2};
    const LatLon t = w.direct(a, 90.0, 2000.0).point;
    const LatLon e0 = w.direct(a, 90.0, 200.0).point, e1 = w.direct(a, 90.0, 1800.0).point;
    const json area = {{{"lat", 48.4996}, {"lon", e0.lon}}, {{"lat", 48.5004}, {"lon", e0.lon}},
                       {{"lat", 48.5004}, {"lon", e1.lon}}, {{"lat", 48.4996}, {"lon", e1.lon}}};
    const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
                          {"returnToPilot", false}, {"evaluate", "control"}};
    const DistanceLink link(w);
    const json r = relay::analyze({{"mission", mission}, {"options", {{"area", area}, {"cellM", 50.0}, {"stepM", 500.0}}}}, w, link);
    const json& best = r.at("data").at("candidates").at(0);
    // Hop 1 is pilot→relay, hop 2 the farthest route sample from the relay. Minimax is the middle.
    EXPECT_NEAR(w.inverse(a, {best.at("lat"), best.at("lon")}).distanceM, 1000.0, 40.0);
    EXPECT_NEAR(best.at("scoreDb").get<double>(), 0.0, 0.4);
    EXPECT_NEAR(r.at("data").at("direct").at("worstMarginDb").get<double>(), -10.0, 1e-6);
    EXPECT_TRUE(r.at("reasons").back().contains("fix"));
}
