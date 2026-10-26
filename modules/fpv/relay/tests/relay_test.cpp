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

namespace {

const LocalWorld& flat() {
    static const LocalWorld w([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    return w;
}

json missionTo(double km) {
    const LatLon a{48.5, 35.2};
    const LatLon t = flat().direct(a, 90.0, km * 1000.0).point;
    return {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
            {"returnToPilot", false}, {"evaluate", "control"}};
}

json relayAt(double km) {
    const LatLon p = flat().direct({48.5, 35.2}, 90.0, km * 1000.0).point;
    return {{"lat", p.lat}, {"lon", p.lon}, {"aglM", 10.0}};
}

json areaEast(double fromKm, double toKm) {
    const double w = flat().direct({48.5, 35.2}, 90.0, fromKm * 1000.0).point.lon;
    const double e = flat().direct({48.5, 35.2}, 90.0, toKm * 1000.0).point.lon;
    return {{{"lat", 48.4996}, {"lon", w}}, {{"lat", 48.5004}, {"lon", w}}, {{"lat", 48.5004}, {"lon", e}}, {{"lat", 48.4996}, {"lon", e}}};
}

}  // namespace

TEST(Relay, WithoutRelayOrAreaAsksForOne) {
    const DistanceLink link(flat());
    const json r = relay::analyze({{"mission", missionTo(1.0)}}, flat(), link);
    ASSERT_EQ(r.at("reasons").size(), 1u);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
    EXPECT_FALSE(r.at("data").contains("chosen"));
    EXPECT_NEAR(r.at("data").at("direct").at("worstMarginDb").get<double>(), 0.0, 1e-6);
}

TEST(Relay, ChosenRelayHalfWayClosesTheLink) {
    const DistanceLink link(flat());
    const json r = relay::analyze({{"mission", missionTo(1.6)}, {"options", {{"relay", relayAt(0.8)}, {"stepM", 400.0}}}}, flat(), link);
    const json& chosen = r.at("data").at("chosen");
    EXPECT_NEAR(chosen.at("pilotToRelay").at("worstMarginDb").get<double>(), 2.0, 1e-6);
    EXPECT_NEAR(chosen.at("relayToDrone").at("worstMarginDb").get<double>(), 2.0, 1e-6);
    EXPECT_NEAR(chosen.at("scoreDb").get<double>(), 2.0, 1e-6);
    EXPECT_EQ(chosen.at("relayToDrone").at("sample"), 3);
    // A placed relay and no area: only the relay's own line, no prompt to place one.
    ASSERT_EQ(r.at("reasons").size(), 1u);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
}

TEST(Relay, ChosenRelayThatLeavesAHopShortIsBlocked) {
    const DistanceLink link(flat());
    const json r = relay::analyze({{"mission", missionTo(2.0)}, {"options", {{"relay", relayAt(0.1)}}}}, flat(), link);
    EXPECT_NEAR(r.at("data").at("chosen").at("scoreDb").get<double>(), -9.0, 1e-6);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "blocked");
}

TEST(Relay, AreaThatCannotBeatTheChosenRelaySuggestsNothing) {
    const DistanceLink link(flat());
    // The chosen relay is the middle already; the area lies near the pilot.
    const json r = relay::analyze({{"mission", missionTo(2.0)},
                                   {"options", {{"relay", relayAt(1.0)}, {"area", areaEast(0.1, 0.4)}, {"cellM", 100.0}}}},
                                  flat(), link);
    EXPECT_FALSE(r.at("data").at("candidates").empty());
    ASSERT_EQ(r.at("reasons").size(), 1u);
    EXPECT_FALSE(r.at("reasons").at(0).contains("fix"));
}

TEST(Relay, AreaWithoutTerrainIsAWarning) {
    const LocalWorld holes([](const LatLon& p) { return p.lon > 35.205 ? std::nullopt : std::optional<double>(100.0); }, 30.0);
    const DistanceLink link(holes);
    const json r = relay::analyze({{"mission", missionTo(2.0)}, {"options", {{"area", areaEast(0.5, 1.5)}}}}, holes, link);
    EXPECT_TRUE(r.at("data").at("candidates").empty());
    EXPECT_EQ(r.at("reasons").back().at("severity"), "warn");
}

TEST(Relay, EvaluateRelayTakesTheWorstLinkOnTheFirstHop) {
    class PerLink final : public ILinkEvaluator {
    public:
        double marginDb(const std::string& link, const End&, const End&) const override { return link == "video" ? -3.0 : 4.0; }
    };
    const PerLink link;
    const Mission m = Mission::fromJson(missionTo(1.0));
    const Route route(m, flat());
    const End pilot{m.pilot.position, 1.5};
    const relay::Hops h = relay::evaluateRelay(link, {"control", "video"}, pilot, {{48.5, 35.205}, 2.0}, routeSamples(route, 500.0));
    EXPECT_EQ(h.first.link, "video");
    EXPECT_DOUBLE_EQ(h.first.marginDb, -3.0);
    EXPECT_DOUBLE_EQ(h.score(), -3.0);
}

TEST(Relay, NeedsALinkToPlan) {
    const DistanceLink link(flat());
    json m = missionTo(2.0);
    m["evaluate"] = "none";
    const json r = relay::analyze({{"mission", m}, {"options", {{"relay", relayAt(1.0)}}}}, flat(), link);
    ASSERT_EQ(r.at("reasons").size(), 1u);
    EXPECT_EQ(r.at("reasons").at(0).at("severity"), "info");
    EXPECT_TRUE(r.at("data").empty());
}
