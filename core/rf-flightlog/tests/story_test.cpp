// Mode bands, the headline numbers and the story of the flight.

#include "flight_fixture.hpp"

#include <rfflightlog/story.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace rfflightlog;

namespace {

struct Built {
    FlightLog log;
    Timeline timeline;
    std::vector<ModeBand> bands;
};

Built buildFrom(const json& doc) {
    Built b;
    b.log = FlightLog::fromJson(doc.at("logs").at(0));
    b.timeline = TimelineBuilder({}).build(b.log);
    b.bands = ModeBandBuilder().build(b.log);
    return b;
}

std::vector<StoryEvent> ofType(const std::vector<StoryEvent>& story, const std::string& type) {
    std::vector<StoryEvent> out;
    std::ranges::copy_if(story, std::back_inserter(out), [&type](const StoryEvent& e) { return e.type == type; });
    return out;
}

}  // namespace

TEST(ModeBandBuilder, NamesTheBandOfAState) {
    EXPECT_EQ(ModeBandBuilder::bandOf(false, {"ANGLE"}, "IDLE"), "DISARMED");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"ANGLE"}, "RX_LOSS_DETECTED"), "FAILSAFE");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {}, "GPS_RESCUE"), "GPS_RESCUE");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"ANGLE", "GPSRESCUE"}, "IDLE"), "GPS_RESCUE");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"ANGLE", "POSHOLD"}, "IDLE"), "POSHOLD");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"ALTHOLD"}, "IDLE"), "ALTHOLD");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"HORIZON"}, "IDLE"), "HORIZON");
    EXPECT_EQ(ModeBandBuilder::bandOf(true, {"AIRMODE"}, "IDLE"), "ACRO");
}

TEST(ModeBandBuilder, SplitsTheFlightIntoBands) {
    const Built b = buildFrom(test::FlightFixture().build());
    ASSERT_EQ(b.bands.size(), 4u);
    EXPECT_EQ(b.bands[0].band, "DISARMED");
    EXPECT_EQ(b.bands[1].band, "ANGLE");
    EXPECT_DOUBLE_EQ(b.bands[1].from, 2.0);
    EXPECT_EQ(b.bands[2].band, "ACRO");
    EXPECT_DOUBLE_EQ(b.bands[2].to, 50.0);
    EXPECT_EQ(b.bands[3].band, "DISARMED");
    EXPECT_DOUBLE_EQ(b.bands[3].to, 60.0);
    EXPECT_EQ(ModeBandBuilder::toJson(b.bands).size(), 4u);
}

TEST(SummaryBuilder, AddsUpTheFlight) {
    const Built b = buildFrom(test::FlightFixture().build());
    const FlightSummary s = SummaryBuilder().build(b.log, b.timeline, b.bands);
    EXPECT_DOUBLE_EQ(s.armedS, 48.0);
    EXPECT_EQ(s.cells, 4);
    EXPECT_NEAR(s.usedMah.value_or(-1.0), 166.67, 0.01);
    EXPECT_NEAR(s.distanceM.value_or(-1.0), 480.0, 2.0);
    EXPECT_NEAR(s.maxHomeDistM.value_or(-1.0), 480.0, 2.0);
    EXPECT_NEAR(s.maxSpeedMps.value_or(-1.0), 10.0, 1e-9);
    EXPECT_NEAR(s.minLinkPct.value_or(-1.0), 20.0, 1e-9);
    EXPECT_EQ(s.linkChannel, "rssiPct");
    EXPECT_NEAR(s.startVoltageV.value_or(-1.0), 16.8, 1e-9);
    EXPECT_EQ(s.disarmReason.value_or(""), "SWITCH");
    const json j = s.toJson();
    EXPECT_EQ(j.at("cells"), 4);
    EXPECT_EQ(j.at("linkChannel"), "rssiPct");
}

TEST(SummaryBuilder, HidesEnergyWithoutACurrentSensor) {
    test::FlightFixture f;
    f.withCurrent = false;
    const Built b = buildFrom(f.build());
    const json j = SummaryBuilder().build(b.log, b.timeline, b.bands).toJson();
    EXPECT_FALSE(j.contains("usedMah"));
    EXPECT_FALSE(j.contains("usedWh"));
    EXPECT_FALSE(j.contains("maxCurrentA"));
}

TEST(StoryBuilder, TellsTheFlight) {
    const Built b = buildFrom(test::FlightFixture().build());
    const auto story = StoryBuilder({}).build(b.log, b.timeline, b.bands);
    ASSERT_FALSE(story.empty());
    EXPECT_TRUE(std::ranges::is_sorted(story, {}, &StoryEvent::t));

    const auto armed = ofType(story, "armed");
    ASSERT_EQ(armed.size(), 1u);
    EXPECT_EQ(armed[0].title, "Armed");
    EXPECT_DOUBLE_EQ(armed[0].t, 2.0);

    const auto mode = ofType(story, "mode");
    ASSERT_EQ(mode.size(), 1u);
    EXPECT_EQ(mode[0].title, "Mode: Acro");
    EXPECT_EQ(mode[0].detail, "from Angle");

    const auto weak = ofType(story, "link-low");
    ASSERT_EQ(weak.size(), 1u);
    EXPECT_NEAR(weak[0].t, 30.0, 0.11);
    EXPECT_EQ(weak[0].title, "Weak link: RSSI low");
    EXPECT_NE(weak[0].detail.find("lowest 20 %"), std::string::npos) << weak[0].detail;
    ASSERT_TRUE(weak[0].lat.has_value());

    const auto disarmed = ofType(story, "disarmed");
    ASSERT_EQ(disarmed.size(), 1u) << "the logged reason replaces the plain state change";
    EXPECT_EQ(disarmed[0].detail, "disarmed with the arm switch");
    EXPECT_EQ(ofType(story, "farthest").size(), 1u);
    EXPECT_EQ(ofType(story, "fastest")[0].detail, "36 km/h over ground");
    EXPECT_TRUE(ofType(story, "log-stops-armed").empty());
    EXPECT_TRUE(ofType(story, "low-cell").empty());
}

TEST(StoryBuilder, PrefersLinkQualityAndFindsLowCells) {
    test::FlightFixture f;
    f.withLinkQuality = true;
    const Built b = buildFrom(f.build());
    StoryOptions o;
    o.lowCellV = 4.0;
    const auto story = StoryBuilder(o).build(b.log, b.timeline, b.bands);
    EXPECT_EQ(ofType(story, "link-low").at(0).title, "Weak link: Link quality low");
    const auto low = ofType(story, "low-cell");
    ASSERT_EQ(low.size(), 1u);
    EXPECT_NEAR(low[0].t, (16.8 - 16.0) / 1.8 * 60.0, 0.15);
}

TEST(StoryBuilder, WeakLinkOnTheGroundIsNotAMoment) {
    test::FlightFixture f;
    f.lowFromS = 52.0;
    f.lowToS = 58.0;
    const Built b = buildFrom(f.build());
    EXPECT_TRUE(ofType(StoryBuilder({}).build(b.log, b.timeline, b.bands), "link-low").empty());
}

TEST(StoryBuilder, NamesFailsafeRxLossGapsAndALogThatStopsArmed) {
    json doc = test::FlightFixture().build();
    json& log = doc["logs"][0];
    log["states"].erase(log["states"].size() - 1);  // never disarmed
    log["states"].push_back({{"t", 40.0}, {"failsafe", "RX_LOSS_DETECTED"}, {"rxSignal", false}});
    log["states"].push_back({{"t", 41.0}, {"failsafe", "IDLE"}, {"rxSignal", true}});
    log["events"] = json::array({{{"t", 10.0}, {"type", "gap"}, {"reason", "corrupt"}, {"durationS", 0.13}},
                                 {{"t", 12.0}, {"type", "logging-paused"}, {"durationS", 4.0}},
                                 {{"t", 44.6}, {"type", "disarm"}, {"reason", "SWITCH"}},
                                 {{"t", 45.0}, {"type", "disarm"}, {"reason", "CRASH_PROTECTION"}}});
    const Built b = buildFrom(doc);
    const auto story = StoryBuilder({}).build(b.log, b.timeline, b.bands);
    const auto fs = ofType(story, "failsafe");
    ASSERT_EQ(fs.size(), 2u);
    EXPECT_EQ(fs[0].severity, "blocked");
    EXPECT_EQ(fs[0].detail, "receiver signal lost, failsafe started");
    EXPECT_EQ(fs[1].title, "Failsafe over");
    EXPECT_EQ(ofType(story, "rx-loss").size(), 2u);
    EXPECT_EQ(ofType(story, "gap").at(0).detail, "0.13 s of damaged data skipped");
    EXPECT_EQ(ofType(story, "paused").at(0).detail, "for 4.0 s");
    ASSERT_EQ(ofType(story, "disarmed").size(), 1u) << "of two disarms at the same moment the later one is told";
    EXPECT_EQ(ofType(story, "disarmed").at(0).severity, "blocked");
    EXPECT_TRUE(ofType(story, "log-stops-armed").empty()) << "the disarm was logged";
    EXPECT_TRUE(std::ranges::none_of(ofType(story, "mode"), [](const StoryEvent& e) { return e.title == "Mode: Failsafe"; }));

    log["events"].erase(3);
    log["events"].erase(2);
    const Built cut = buildFrom(doc);
    EXPECT_EQ(ofType(StoryBuilder({}).build(cut.log, cut.timeline, cut.bands), "log-stops-armed").size(), 1u);
}

TEST(StoryBuilder, OlderFirmwareWithoutDisarmEventsStillDisarms) {
    test::FlightFixture f;
    f.disarmReason.clear();
    const Built b = buildFrom(f.build());
    const auto disarmed = ofType(StoryBuilder({}).build(b.log, b.timeline, b.bands), "disarmed");
    ASSERT_EQ(disarmed.size(), 1u);
    EXPECT_TRUE(disarmed[0].detail.empty());
    EXPECT_EQ(StoryBuilder::disarmText("NEW_REASON"), "reason NEW_REASON");
    EXPECT_EQ(StoryBuilder::bandTitle("POSHOLD"), "Position hold");
}
