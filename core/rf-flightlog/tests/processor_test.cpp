// The processor interface: a normalized document in, one replay view per flight out.

#include "flight_fixture.hpp"

#include <rfflightlog/processor.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

using namespace rfflightlog;

TEST(FlightLogProcessor, TurnsEachFlightIntoAReplayView) {
    const FlightLogProcessor processor;
    const IFlightLogProcessor& api = processor;
    json doc = test::FlightFixture().build();
    doc["logs"].push_back({{"index", 1}, {"status", "header-only"}, {"firmware", json::object()},
                           {"channels", json::object()},
                           {"notes", json::array({{{"code", "header-only"}, {"severity", "warn"}, {"message", "x"}}})}});
    const auto flights = api.process(doc, {});
    ASSERT_EQ(flights.size(), 2u);

    const json view = flights[0].toJson();
    EXPECT_EQ(view.at("schema"), "flightview/v1");
    EXPECT_EQ(view.at("craft"), "Fixture");
    EXPECT_EQ(view.at("bands").size(), 4u);
    EXPECT_TRUE(view.at("timeline").at("channels").contains("lat"));
    EXPECT_EQ(view.at("homes").size(), 1u);
    EXPECT_FALSE(view.at("story").empty());
    EXPECT_EQ(view.at("warnings"), json::array({"Weak link: RSSI low"}));

    const json row = flights[1].summaryJson();
    EXPECT_EQ(row.at("status"), "header-only");
    EXPECT_FALSE(row.contains("timeline"));
    EXPECT_TRUE(flights[1].story.empty());
    EXPECT_EQ(flights[1].toJson().at("notes").size(), 1u);

    const json c = api.compare(flights[0], RoutePlan::fromMission(
        {{"pilot", {{"lat", 50.0}, {"lon", 30.0}, {"aglM", 0}}}, {"waypoints", json::array({{{"lat", 50.0}, {"lon", 30.01}, {"aglM", 30}}})}},
        std::nullopt));
    EXPECT_TRUE(c.contains("rows"));
}

TEST(FlightLogProcessor, RejectsWhatItCannotProcess) {
    const FlightLogProcessor processor;
    EXPECT_THROW((void)processor.process(json{{"schema", "other"}}, {}), std::invalid_argument);
    ProcessOptions fast;
    fast.timelineHz = 0.0;
    EXPECT_THROW((void)processor.process(test::FlightFixture().build(), fast), std::invalid_argument);
}

// A normalized document of a real flight, when given: FLIGHTLOG_NORMALIZED_JSON names the file
// (fpv.blackbox's answer for a log), so a real log can be checked without committing it.
TEST(FlightLogProcessor, RealFlightFromTheEnvironment) {
    const char* path = std::getenv("FLIGHTLOG_NORMALIZED_JSON");
    if (path == nullptr) GTEST_SKIP() << "FLIGHTLOG_NORMALIZED_JSON is not set";
    std::ifstream in(path);
    const json doc = json::parse(in);
    const auto flights = FlightLogProcessor().process(doc, {});
    ASSERT_FALSE(flights.empty());
    for (const ProcessedFlight& f : flights) {
        const json view = f.toJson();
        EXPECT_EQ(view.at("schema"), "flightview/v1");
        std::cout << view.at("summary").dump() << "\n";
        for (const json& e : view.at("story")) std::cout << "  " << e.dump() << "\n";
    }
}
