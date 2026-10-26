// The bounds the server puts on a request before it does the work: route size, body size.

#include "request.hpp"
#include "route_workload.hpp"

#include <gtest/gtest.h>

using namespace rfmap;

namespace {

json point(double lat, double lon) { return {{"lat", lat}, {"lon", lon}, {"aglM", 50}}; }

json route(const json& waypoints, bool returnToPilot) {
    return {{"pilot", point(48.50, 35.20)}, {"waypoints", waypoints}, {"returnToPilot", returnToPilot}};
}

}  // namespace

TEST(RouteWorkloadTest, CountsThePositionsTheEvaluatorSamples) {
    // About 1 km north of the pilot, sampled every 100 m: 11 or 12 positions out, as many back.
    const json m = route(json::array({point(48.509, 35.20)}), true);
    const RouteWorkload out(m, 100.0, false);
    EXPECT_GE(out.positions(), 22.0);
    EXPECT_LE(out.positions(), 24.0);
    EXPECT_EQ(out.samples(), out.positions());  // without links only the ground is read
    EXPECT_NEAR(RouteWorkload(route(json::array({point(48.509, 35.20)}), false), 100.0, false).positions(),
                out.positions() / 2.0, 0.5);
}

TEST(RouteWorkloadTest, CountsTheGroundReadsOfLaunchAltitudes) {
    json m = route(json::array({point(48.509, 35.20)}), false);
    const RouteWorkload agl(m, 100.0, false);
    m["altitudeRef"] = "launch";
    const RouteWorkload launch(m, 100.0, false);
    // One more read per position for the ground under the drone, one per leg at the pilot.
    EXPECT_EQ(launch.positions(), agl.positions());
    EXPECT_EQ(launch.samples(), 2.0 * agl.samples() + 1.0);
}

TEST(RouteWorkloadTest, BoundsTheProfilesFromThePilot) {
    const json m = route(json::array({point(48.509, 35.20)}), false);
    const RouteWorkload w(m, 100.0, true);
    // Every position builds a profile from the pilot of at most the leg's length.
    EXPECT_GT(w.samples(), w.positions());
    EXPECT_LE(w.samples(), w.positions() * (1000.0 / 100.0 + 2.0));
}

TEST(RouteWorkloadTest, CountsACoincidentLegAsOnePosition) {
    // A waypoint on the pilot: the evaluator samples that leg at its one point.
    const RouteWorkload w(route(json::array({point(48.50, 35.20)}), false), 100.0, false);
    EXPECT_EQ(w.positions(), 1.0);
    EXPECT_EQ(w.samples(), 1.0);
}

TEST(RouteWorkloadTest, CountsThreeReadsForAShortProfile) {
    // 50 m out with a 100 m step: two positions, each profile reads both ends and the middle.
    const RouteWorkload w(route(json::array({point(48.50045, 35.20)}), false), 100.0, true);
    EXPECT_EQ(w.positions(), 2.0);
    EXPECT_EQ(w.samples(), 6.0);
}

TEST(RouteWorkloadTest, RefusesARouteOverTheLimitWithTheNumbers) {
    // Two legs across a continent: tens of thousands of positions, each a profile as long.
    const json far = route(json::array({point(40.0, 20.0)}), true);
    const RouteWorkload w(far, 30.0, true);
    EXPECT_GT(w.samples(), 1e9);
    try {
        w.require(20e6);
        FAIL() << "a continental route must be refused";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.status(), 422);
        EXPECT_GT(e.body().at("samples").get<double>(), 20e6);
        EXPECT_EQ(e.body().at("maxSamples").get<double>(), 20e6);
    }
    EXPECT_NO_THROW(RouteWorkload(route(json::array({point(48.509, 35.20)}), true), 30.0, true).require(20e6));
}

TEST(BodyLimitTest, OnlyFlightLogRoutesTakeFiles) {
    Config::Server server;
    server.maxBodyBytes = 1000;
    server.maxUploadBytes = 50000;
    const BodyLimit limit(server);
    EXPECT_EQ(limit.forPath("/api/v1/missions"), 1000u);
    EXPECT_EQ(limit.forPath("/api/v1/flightlogs/decode"), 50000u);
    EXPECT_EQ(limit.forPath("/api/v1/missions/abc/flightlogs"), 50000u);
    EXPECT_EQ(limit.forPath("/api/v1/missions/abc/flightlogs/xyz"), 1000u);

    EXPECT_FALSE(limit.exceeded("/api/v1/missions", "1000"));
    EXPECT_TRUE(limit.exceeded("/api/v1/missions", "1001"));
    EXPECT_FALSE(limit.exceeded("/api/v1/flightlogs/decode", "1001"));
    EXPECT_TRUE(limit.exceeded("/api/v1/flightlogs/decode", "50001"));
    EXPECT_TRUE(limit.exceeded("/api/v1/missions", "99999999999999999999999999"));
    // Not a length: httplib answers those itself.
    EXPECT_FALSE(limit.exceeded("/api/v1/missions", ""));
    EXPECT_FALSE(limit.exceeded("/api/v1/missions", "12ab"));
    EXPECT_FALSE(limit.exceeded("/api/v1/missions", "-5"));

    // Compressed bodies inflate past every limit.
    EXPECT_TRUE(BodyLimit::encodingRefused("gzip"));
    EXPECT_TRUE(BodyLimit::encodingRefused("BR"));
    EXPECT_FALSE(BodyLimit::encodingRefused(""));
    EXPECT_FALSE(BodyLimit::encodingRefused("Identity"));

    // A chunked JSON body has no length to judge; a chunked flight log is bounded by httplib.
    EXPECT_TRUE(limit.lengthRequired("/api/v1/missions", "chunked"));
    EXPECT_FALSE(limit.lengthRequired("/api/v1/missions", ""));
    EXPECT_FALSE(limit.lengthRequired("/api/v1/flightlogs/decode", "chunked"));
    // By route, not by which limit is larger.
    server.maxBodyBytes = 100000;
    EXPECT_TRUE(limit.lengthRequired("/api/v1/missions", "chunked"));
}
