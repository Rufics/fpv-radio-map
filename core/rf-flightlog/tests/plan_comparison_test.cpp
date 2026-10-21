// Plan against fact: the route read from a mission and its last result, the distance off the
// route, and the rows and findings the comparison gives.

#include "flight_fixture.hpp"

#include <rfflightlog/plan_comparison.hpp>

#include <gtest/gtest.h>

using namespace rfflightlog;
using test::FlightFixture;

namespace {

json mission(double northOffsetM, bool returnToPilot) {
    const double dLat = northOffsetM / 111'200.0;
    return {{"schema", "mission/v1"},
            {"pilot", {{"lat", FlightFixture::kHomeLat + dLat}, {"lon", FlightFixture::kHomeLon}, {"aglM", 0}}},
            {"waypoints", json::array({{{"lat", FlightFixture::kHomeLat + dLat},
                                        {"lon", FlightFixture::kHomeLon + 500.0 / FlightFixture::kMetresPerDegLon},
                                        {"aglM", 30},
                                        {"label", "Bridge"}}})},
            {"returnToPilot", returnToPilot}};
}

json routeResult() {
    return {{"legs", json::array({{{"index", 0}, {"lengthM", 500}, {"worst", {{"control", {{"marginDb", 12.0}}}}}}})},
            {"modules", {{"fpv.battery", {{"neededWh", 2.0}}}}}};
}

struct Flight {
    FlightLog log;
    Timeline timeline;
    FlightSummary summary;
};

Flight fly(const FlightFixture& f) {
    Flight out;
    out.log = FlightLog::fromJson(f.build().at("logs").at(0));
    out.timeline = TimelineBuilder({}).build(out.log);
    out.summary = SummaryBuilder().build(out.log, out.timeline, ModeBandBuilder().build(out.log));
    return out;
}

const json& rowOf(const json& rows, const std::string& metric) {
    for (const json& r : rows) {
        if (r.at("metric") == metric) return r;
    }
    throw std::out_of_range(metric);
}

bool anyFinding(const json& findings, const std::string& severity, const std::string& part) {
    for (const json& f : findings) {
        if (f.at("severity") == severity && f.at("text").get<std::string>().find(part) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(RoutePlan, ReadsTheMissionAndItsResult) {
    const RoutePlan out = RoutePlan::fromMission(mission(0, true), routeResult());
    ASSERT_EQ(out.points.size(), 3u);
    ASSERT_EQ(out.legs.size(), 2u);
    EXPECT_EQ(out.legs[0].from, "Pilot");
    EXPECT_EQ(out.legs[0].to, "Bridge");
    EXPECT_EQ(out.legs[1].to, "Pilot");
    EXPECT_NEAR(out.legs[0].lengthM, 500.0, 2.0);
    EXPECT_NEAR(out.lengthM(), 1000.0, 4.0);
    EXPECT_DOUBLE_EQ(out.legs[0].controlMarginDb.value_or(0.0), 12.0);
    EXPECT_FALSE(out.legs[1].controlMarginDb.has_value());
    EXPECT_FALSE(out.legs[0].videoMarginDb.has_value());
    EXPECT_DOUBLE_EQ(out.neededWh.value_or(0.0), 2.0);
    EXPECT_FALSE(RoutePlan::fromMission(mission(0, false), std::nullopt).neededWh.has_value());
}

TEST(RouteGeometry, MeasuresTheDistanceToTheNearestLeg) {
    const RouteGeometry route({{50.0, 30.0}, {50.0, 30.01}, {50.01, 30.01}});
    if (const auto onFirst = route.nearest(50.0009, 30.005)) {
        EXPECT_EQ(onFirst->leg, 0u);
        EXPECT_NEAR(onFirst->distanceM, 100.0, 1.0);
    } else {
        ADD_FAILURE() << "no leg near the first point";
    }
    if (const auto onSecond = route.nearest(50.005, 30.0115)) {
        EXPECT_EQ(onSecond->leg, 1u);
    } else {
        ADD_FAILURE() << "no leg near the second point";
    }
    EXPECT_FALSE(RouteGeometry({{50.0, 30.0}}).nearest(50.0, 30.0).has_value());
}

TEST(PlanComparison, ComparesARouteFlownAsPlanned) {
    const Flight f = fly(FlightFixture());
    const json c = PlanComparison({}).compare(RoutePlan::fromMission(mission(0, false), routeResult()), f.timeline, f.summary);
    EXPECT_NEAR(rowOf(c.at("rows"), "distance").at("plan").get<double>(), 500.0, 2.0);
    EXPECT_NEAR(rowOf(c.at("rows"), "distance").at("fact").get<double>(), 480.0, 2.0);
    EXPECT_NEAR(rowOf(c.at("rows"), "distance").at("deltaPct").get<double>(), -4.0, 0.5);
    EXPECT_DOUBLE_EQ(rowOf(c.at("rows"), "energy").at("plan").get<double>(), 2.0);
    EXPECT_FALSE(rowOf(c.at("rows"), "weakestLink").contains("deltaPct"));
    EXPECT_LT(c.at("furthest").at("distanceM").get<double>(), 1.0);
    EXPECT_EQ(c.at("legs").size(), 1u);
    EXPECT_NEAR(c.at("legs")[0].at("flownS").get<double>(), 48.0, 0.2);
    EXPECT_DOUBLE_EQ(c.at("legs")[0].at("minLinkPct").get<double>(), 20.0);
    EXPECT_EQ(c.at("offRouteM").size(), f.timeline.size());
    EXPECT_TRUE(c.at("offRouteM")[0].is_null()) << "on the ground before arming";
    EXPECT_TRUE(anyFinding(c.at("findings"), "info", "Flew 0.48 km against 0.50 km planned (-4 %)"));
    EXPECT_TRUE(anyFinding(c.at("findings"), "warn", "planned 12 dB of control margin, but the link fell to 20 %"));
    EXPECT_TRUE(anyFinding(c.at("findings"), "warn", "Wh planned"));
}

TEST(PlanComparison, FindsTheCraftOffTheRoute) {
    const Flight f = fly(FlightFixture());
    const json c = PlanComparison({}).compare(RoutePlan::fromMission(mission(100, false), routeResult()), f.timeline, f.summary);
    EXPECT_NEAR(c.at("furthest").at("distanceM").get<double>(), 100.0, 1.5);
    EXPECT_TRUE(anyFinding(c.at("findings"), "warn", "Furthest off the route: 100 m, on the leg Pilot → Bridge"));
}

TEST(PlanComparison, SaysWhatCannotBeCompared) {
    FlightFixture fixture;
    fixture.withCurrent = false;
    fixture.withGps = false;
    const Flight f = fly(fixture);
    const json c = PlanComparison({}).compare(RoutePlan::fromMission(mission(0, false), routeResult()), f.timeline, f.summary);
    EXPECT_TRUE(rowOf(c.at("rows"), "energy").at("fact").is_null());
    EXPECT_FALSE(c.contains("furthest"));
    EXPECT_TRUE(anyFinding(c.at("findings"), "info", "no current sensor"));
    EXPECT_TRUE(anyFinding(c.at("findings"), "info", "no GPS track"));
}
