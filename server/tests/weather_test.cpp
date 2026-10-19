#include "weather.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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
class SourceDown : public ForecastUnavailableError {
public:
    SourceDown() : ForecastUnavailableError("connection refused") {}
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

namespace {

/// A local stand-in for Open-Meteo whose answers each test sets: a status and a body per path.
class LocalOpenMeteo {
public:
    LocalOpenMeteo() {
        server_.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard lock(mutex_);
            lastQuery_ = req.target;
            const auto it = answers_.find(req.path);
            if (it == answers_.end()) {
                res.status = 404;
                return;
            }
            res.status = it->second.first;
            res.set_content(it->second.second, "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~LocalOpenMeteo() {
        server_.stop();
        thread_.join();
    }
    LocalOpenMeteo(const LocalOpenMeteo&) = delete;
    LocalOpenMeteo& operator=(const LocalOpenMeteo&) = delete;

    void answer(const std::string& path, int status, const std::string& body) {
        std::lock_guard lock(mutex_);
        answers_[path] = {status, body};
    }
    [[nodiscard]] std::string lastQuery() const {
        std::lock_guard lock(mutex_);
        return lastQuery_;
    }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<int, std::string>, std::less<>> answers_;
    std::string lastQuery_;
};

/// Open-Meteo settings pointing at `base`, with a short timeout.
Config::Weather openMeteoAt(const std::string& base, const std::string& metaUrl = "") {
    Config::Weather cfg;
    cfg.baseUrl = base;
    cfg.metaUrl = metaUrl;
    cfg.timeoutMs = 1500;
    return cfg;
}

/// The message `f` fails with as ForecastUnavailableError; empty when it does not.
template <class F>
std::string unavailable(F&& f) {
    try {
        std::forward<F>(f)();
    } catch (const ForecastUnavailableError& e) {
        return e.what();
    }
    return {};
}

}  // namespace

TEST(OpenMeteoSourceTest, FetchAsksForTheColumnAndTheModels) {
    LocalOpenMeteo service;
    service.answer("/v1/forecast", 200, forecast(kNoon).dump());
    OpenMeteoSource source(openMeteoAt(service.url()));
    EXPECT_EQ(source.fetch(50.5, 30.5, "icon_d2"), forecast(kNoon));
    const std::string query = service.lastQuery();
    EXPECT_NE(query.find("latitude=50.5&longitude=30.5"), std::string::npos) << query;
    EXPECT_NE(query.find("geopotential_height_850hPa"), std::string::npos) << query;
    EXPECT_NE(query.find("&models=icon_d2"), std::string::npos) << query;
    (void)source.fetch(50.5, 30.5, "");
    EXPECT_EQ(service.lastQuery().find("models="), std::string::npos);
}

TEST(OpenMeteoSourceTest, AnErrorStatusNamesTheReasonWhenTheBodyGivesOne) {
    LocalOpenMeteo service;
    OpenMeteoSource source(openMeteoAt(service.url()));
    service.answer("/v1/forecast", 400, json{{"error", true}, {"reason", "Latitude must be in range"}}.dump());
    EXPECT_EQ(unavailable([&] { (void)source.fetch(0, 0, ""); }),
              "forecast service answered HTTP 400: Latitude must be in range");
    service.answer("/v1/forecast", 503, "busy");
    EXPECT_EQ(unavailable([&] { (void)source.fetch(0, 0, ""); }), "forecast service answered HTTP 503");
    service.answer("/v1/forecast", 500, json{{"reason", 7}}.dump());
    EXPECT_EQ(unavailable([&] { (void)source.fetch(0, 0, ""); }), "forecast service answered HTTP 500");
}

TEST(OpenMeteoSourceTest, AnUnreachableServiceIsUnavailable) {
    std::string url;
    {
        const LocalOpenMeteo gone;
        url = gone.url();
    }
    OpenMeteoSource source(openMeteoAt(url));
    EXPECT_NE(unavailable([&] { (void)source.fetch(0, 0, ""); }).find("forecast service unreachable"),
              std::string::npos);
}

TEST(OpenMeteoSourceTest, RunTimeComesFromTheModelMetadata) {
    LocalOpenMeteo service;
    EXPECT_FALSE(OpenMeteoSource(openMeteoAt(service.url())).runTime().has_value());

    OpenMeteoSource source(openMeteoAt(service.url(), service.url() + "/data/icon_d2/static/meta.json"));
    service.answer("/data/icon_d2/static/meta.json", 200, json{{"last_run_initialisation_time", kNoon}}.dump());
    EXPECT_EQ(source.runTime(), Clock::from_time_t(kNoon));
    service.answer("/data/icon_d2/static/meta.json", 200, json{{"other", 1}}.dump());
    EXPECT_FALSE(source.runTime().has_value());
}

TEST(OpenMeteoSourceTest, AnUnreadableRunTimeLeavesTheRunUnknown) {
    for (const std::string& meta : {std::string("not json"), json{{"last_run_initialisation_time", "noon"}}.dump()}) {
        LocalOpenMeteo service;
        service.answer("/v1/forecast", 200, forecast(kNoon).dump());
        service.answer("/meta.json", 200, meta);
        const Config::Weather cfg = openMeteoAt(service.url(), service.url() + "/meta.json");
        OpenMeteoSource source(cfg);
        FixedClock clock;
        WeatherService weather(cfg, &source, clock);
        const ForecastAt at = weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
        EXPECT_FALSE(at.point.contains("runUtc")) << meta;
        EXPECT_FALSE(at.point.contains("error")) << meta;
    }
}

TEST(OpenMeteoSourceTest, ABodyThatIsNotJsonFailsTheTier) {
    LocalOpenMeteo service;
    service.answer("/v1/forecast", 200, "<html>");
    const Config::Weather cfg = openMeteoAt(service.url());
    OpenMeteoSource source(cfg);
    FixedClock clock;
    WeatherService weather(cfg, &source, clock);
    const ForecastAt at = weather.at(50.45, 30.52, Clock::from_time_t(kNoon));
    ASSERT_TRUE(at.point.contains("error")) << at.point;
    EXPECT_NE(at.point.at("error").get<std::string>().find("parse"), std::string::npos) << at.point;
}
