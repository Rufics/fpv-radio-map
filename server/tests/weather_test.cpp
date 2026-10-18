#include "weather.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

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

}  // namespace
