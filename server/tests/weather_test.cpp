#include "weather.hpp"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace rfmap;

namespace {

constexpr std::time_t kNoon = 1792324800;  // 2026-10-18T12:00:00Z

class FixedClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const override { return Clock::from_time_t(kNoon); }
};

class CannedSource final : public IForecastSource {
public:
    explicit CannedSource(json response) : response_(std::move(response)) {}
    json fetch(double, double, const std::string&) override { return response_; }
    std::optional<TimePoint> runTime() override { return std::nullopt; }

private:
    json response_;
};

/// The forecast service being unreachable.
class SourceDown : public std::runtime_error {
public:
    SourceDown() : std::runtime_error("connection refused") {}
};

/// A source that answers per model tier, can name a run time, and fails for the tiers told to.
class ScriptedSource final : public IForecastSource {
public:
    json fetch(double, double, const std::string& models) override {
        ++fetches;
        if (down.contains(models)) throw SourceDown();
        return responses.at(models);
    }
    std::optional<TimePoint> runTime() override {
        if (runDown) throw SourceDown();
        return run;
    }
    std::map<std::string, json> responses;
    std::set<std::string> down;
    std::optional<TimePoint> run;
    bool runDown = false;
    int fetches = 0;
};

/// Weather settings with the given model tiers.
Config::Weather withTiers(std::vector<Config::WeatherTier> tiers) {
    Config::Weather cfg;
    cfg.tiers = std::move(tiers);
    return cfg;
}

/// The weather service over a scripted source, by default with the hourly and long-range tiers.
struct TieredRig {
    explicit TieredRig(std::vector<Config::WeatherTier> tiers = {{"hourly", "icon_d2"}, {"long-range", ""}})
        : cfg(withTiers(std::move(tiers))) {}
    FixedClock clock;
    ScriptedSource source;
    Config::Weather cfg;
    WeatherService weather{cfg, &source, clock};
};

/// Three hours of surface values from `first` on, with no pressure levels.
json forecast(std::int64_t first) {
    return {{"elevation", 120.0},
            {"hourly",
             {{"time", {first, first + 3600, first + 7200}},
              {"temperature_2m", {10.0, 11.0, 12.0}},
              {"relative_humidity_2m", {70.0, 65.0, 60.0}},
              {"surface_pressure", {1000.0, 1001.0, 1002.0}}}}};
}

/// The weather service over one canned response, read at a fixed "now".
struct Rig {
    explicit Rig(json response) : source(std::move(response)) {}
    FixedClock clock;
    CannedSource source;
    Config::Weather cfg;
    WeatherService weather{cfg, &source, clock};
};

TEST(WeatherTest, ReadsTheHourNearestTheRequestedTime) {
    Rig rig(forecast(kNoon - 3600));
    const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon + 1200));
    EXPECT_EQ(at.point.at("validUtc"), "2026-10-18T12:00:00Z");
    EXPECT_DOUBLE_EQ(at.point.at("temperatureC").get<double>(), 11.0);
    ASSERT_FALSE(at.notes.empty());
    EXPECT_NE(at.notes.front().detail.find("(20 min from the requested time)"), std::string::npos);
}

// The clock counts nanoseconds in 64 bits, about 292 years: a forecast that starts in 1900 and a
// request for 2199 are further apart than that, and must not overflow it.
TEST(WeatherTest, TimesThreeCenturiesApartAreOutsideTheForecastNotAnOverflow) {
    Rig rig(forecast(-2208988800));                                                     // 1900-01-01
    const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(7258032000));  // 2199-12-31
    EXPECT_EQ(at.point.at("forecastUntilUtc"), "1900-01-01T02:00:00Z");
}

TEST(WeatherTest, NamesTheModelRunAndSaysWhenItIsStale) {
    TieredRig rig;
    rig.source.responses["icon_d2"] = forecast(kNoon - 3600);
    rig.source.run = Clock::from_time_t(kNoon - std::time_t{13} * 3600);
    const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
    EXPECT_EQ(at.point.at("model"), "Open-Meteo icon_d2");
    EXPECT_EQ(at.point.at("runUtc"), "2026-10-17T23:00:00Z");
    EXPECT_TRUE(at.point.at("stale").get<bool>());
    EXPECT_NE(at.sourceLabel.find(", run 2026-10-17T23:00:00Z"), std::string::npos);

    // Fresh enough, and cached: a second read asks the source for nothing.
    TieredRig fresh;
    fresh.source.responses["icon_d2"] = forecast(kNoon - 3600);
    fresh.source.run = Clock::from_time_t(kNoon - 3600);
    EXPECT_FALSE(fresh.weather.at(50.45, 30.52, Clock::from_time_t(kNoon)).point.at("stale").get<bool>());
    (void)fresh.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
    EXPECT_EQ(fresh.source.fetches, 1);
}

TEST(WeatherTest, FallsBackToTheLongRangeTierAndIgnoresAFailingRunTime) {
    TieredRig rig;
    rig.source.down = {"icon_d2"};
    rig.source.runDown = true;
    rig.source.responses[""] = forecast(kNoon);
    const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
    EXPECT_EQ(at.point.at("tier"), "long-range");
    EXPECT_EQ(at.point.at("model"), "Open-Meteo default models");
    EXPECT_FALSE(at.point.contains("runUtc"));
}

TEST(WeatherTest, AnUnreachableServiceIsSaidAndStandardAirUsed) {
    TieredRig rig;
    rig.source.down = {"icon_d2", ""};
    const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
    EXPECT_EQ(at.point.at("tier"), "none");
    EXPECT_EQ(at.point.at("error"), "connection refused");
    ASSERT_FALSE(at.notes.empty());
    EXPECT_NE(at.notes.front().detail.find("forecast unavailable (connection refused)"), std::string::npos);
    EXPECT_EQ(rig.weather.horizon(50.45, 30.52).at("error"), "connection refused");
}

TEST(WeatherTest, ResponsesThatDoNotReadAsAForecastFailTheTier) {
    const std::vector<std::pair<std::string, json>> cases = {
        {"time is not whole seconds", {{"elevation", 1.0}, {"hourly", {{"time", {1.5}}, {"temperature_2m", {1.0}}, {"surface_pressure", {1.0}}}}}},
        {"time out of range", {{"elevation", 1.0}, {"hourly", {{"time", {-9000000000LL}}, {"temperature_2m", {1.0}}, {"surface_pressure", {1.0}}}}}},
    };
    for (const auto& [name, response] : cases) {
        TieredRig rig({{"hourly", ""}});
        rig.source.responses[""] = response;
        const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
        ASSERT_TRUE(at.point.contains("error")) << name << ": " << at.point;
        EXPECT_NE(at.point.at("error").get<std::string>().find("unreadable forecast response"), std::string::npos) << name;
        EXPECT_NE(rig.weather.horizon(50.45, 30.52).value("error", "").find("unreadable forecast response"), std::string::npos)
            << name;
    }
}

TEST(WeatherTest, AForecastWithoutUsableHoursHasNothingToOffer) {
    struct Case {
        std::string name;
        json response;
        bool reaches;  ///< whether the horizon still finds a covered hour
    };
    const std::vector<Case> cases = {
        {"not an object", json::array({1, 2}), false},
        {"no times", {{"elevation", 1.0}, {"hourly", {{"time", "soon"}}}}, false},
        {"no surface pressure", {{"elevation", 1.0}, {"hourly", {{"time", {kNoon}}, {"temperature_2m", {1.0}}}}}, false},
        {"value is text", {{"elevation", 1.0}, {"hourly", {{"time", {kNoon}}, {"temperature_2m", {"warm"}}, {"surface_pressure", {1.0}}}}},
         false},
        {"no elevation",
         {{"hourly", {{"time", {kNoon}}, {"temperature_2m", {1.0}}, {"surface_pressure", {990.0}}, {"relative_humidity_2m", {50.0}}}}},
         true},
    };
    for (const auto& c : cases) {
        TieredRig rig({{"hourly", ""}});
        rig.source.responses[""] = c.response;
        const ForecastAt at = rig.weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
        EXPECT_EQ(at.point.at("tier"), "none") << c.name;
        EXPECT_FALSE(at.point.contains("error")) << c.name;
        EXPECT_EQ(rig.weather.horizon(50.45, 30.52).contains("hourlyUntilUtc"), c.reaches) << c.name;
    }
}

TEST(WeatherTest, HorizonNamesHowFarEachTierReaches) {
    TieredRig rig;
    rig.source.responses["icon_d2"] = forecast(kNoon);
    rig.source.responses[""] = forecast(kNoon + 86400);
    rig.source.run = Clock::from_time_t(kNoon - 3600);
    const json h = rig.weather.horizon(50.45, 30.52);
    EXPECT_EQ(h.at("hourlyUntilUtc"), "2026-10-18T14:00:00Z");
    EXPECT_EQ(h.at("longRangeUntilUtc"), "2026-10-19T14:00:00Z");
    EXPECT_EQ(h.at("runUtc"), "2026-10-18T11:00:00Z");

    Config::Weather cfg;
    FixedClock clock;
    WeatherService none(cfg, nullptr, clock);
    EXPECT_EQ(none.horizon(50.45, 30.52), json({{"configured", false}}));
    EXPECT_NE(none.at(50.45, 30.52, Clock::from_time_t(kNoon)).notes.front().detail.find("no forecast service"),
              std::string::npos);
}

}  // namespace
