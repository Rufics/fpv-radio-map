#include "arguments.hpp"
#include "checks.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace rfmap::cli;

namespace {

json link(const char* kind, double margin, bool reachable = true) {
    return {{"kind", kind}, {"marginDb", margin}, {"reachable", reachable}, {"distanceM", 3700.0}};
}

json leg(int index, const char* status, double control, std::optional<double> video = std::nullopt) {
    json worst = {{"control", link("control", control)}};
    if (video) worst["video"] = link("video", *video);
    return {{"index", index}, {"from", "Pilot"}, {"to", "WP" + std::to_string(index + 1)}, {"lengthM", 3700.0},
            {"status", status}, {"worst", worst}};
}

json route() {
    return {{"legs", {leg(0, "ok", 41.8, 12.3), leg(1, "warn", 6.0), leg(2, "missing-data", 15.0)}},
            {"moduleFailures", {{{"module", "fpv.battery"}, {"error", "timed out"}}}}};
}

}  // namespace

TEST(Checks, SeveritiesOrderTheStatusesAndMissingDataCountsAsBlocked) {
    EXPECT_EQ(legSeverity("ok"), 0);
    EXPECT_EQ(legSeverity("warn"), 1);
    EXPECT_EQ(legSeverity("blocked"), 2);
    EXPECT_EQ(legSeverity("missing-data"), 2);
    EXPECT_EQ(Requirements::severityOf("warn"), 1);
    EXPECT_EQ(Requirements::severityOf("blocked"), 2);
    EXPECT_THROW((void)Requirements::severityOf("ok"), UsageError);
}

TEST(Checks, NoRequirementsNothingFails) { EXPECT_TRUE(routeShortfalls(route(), {}).empty()); }

TEST(Checks, FailOnBlockedCatchesMissingDataButNotWarn) {
    const auto s = routeShortfalls(route(), {.failOnSeverity = 2, .minMarginDb = std::nullopt});
    ASSERT_EQ(s.size(), 1U);
    EXPECT_EQ(s[0], "leg 3 (Pilot -> WP3) is missing-data");
}

TEST(Checks, FailOnWarnCatchesEveryLegThatIsNotOk) {
    EXPECT_EQ(routeShortfalls(route(), {.failOnSeverity = 1, .minMarginDb = std::nullopt}).size(), 2U);
}

TEST(Checks, MinMarginChecksEveryEvaluatedLinkOfEveryLeg) {
    const auto s = routeShortfalls(route(), {.failOnSeverity = std::nullopt, .minMarginDb = 12.5});
    ASSERT_EQ(s.size(), 2U);
    EXPECT_EQ(s[0], "leg 1 (Pilot -> WP1): video margin +12.3 dB is below 12.5 dB");
    EXPECT_EQ(s[1], "leg 2 (Pilot -> WP2): control margin +6.0 dB is below 12.5 dB");
}

TEST(Checks, AResultWithoutLegsFailsOnlyWhenSomethingIsRequired) {
    EXPECT_TRUE(routeShortfalls(json::object(), {}).empty());
    EXPECT_EQ(routeShortfalls(json::object(), {.failOnSeverity = 1, .minMarginDb = std::nullopt}).size(), 1U);
}

TEST(Checks, PathChecksASingleLinkOrBoth) {
    EXPECT_TRUE(pathShortfalls(link("control", 20.0), {.failOnSeverity = 2, .minMarginDb = 10.0}).empty());
    const json both = {{"control", link("control", 20.0)}, {"video", link("video", -3.0, false)}};
    const auto s = pathShortfalls(both, {.failOnSeverity = 2, .minMarginDb = 0.0});
    ASSERT_EQ(s.size(), 2U);
    EXPECT_EQ(s[0], "video margin -3.0 dB is below 0.0 dB");
    EXPECT_EQ(s[1], "video link is not reachable");
}

TEST(Checks, SummariesHaveOneLinePerLegOrLink) {
    const std::string r = routeSummary(route());
    EXPECT_NE(r.find("leg 1 (Pilot -> WP1)"), std::string::npos) << r;
    EXPECT_NE(r.find("control +41.8 dB  video +12.3 dB"), std::string::npos) << r;
    EXPECT_NE(r.find("module fpv.battery failed: timed out"), std::string::npos) << r;
    EXPECT_EQ(std::ranges::count(r, '\n'), 4);
    const std::string p = pathSummary(link("control", -2.5, false));
    EXPECT_NE(p.find("3.70 km  unreachable  margin -2.5 dB"), std::string::npos) << p;
}
