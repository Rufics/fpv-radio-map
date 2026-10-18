// Forecast responses (server weather.hpp): the Open-Meteo JSON is read index by index into the
// air column the refraction and rain terms use. The forecast service is another program on the
// network, so its answers are input like any other.
//
// Input: the /v1/forecast response body. Property: at() and horizon() never throw; a response
// they cannot use means no forecast, not a failed calculation.

#include "../support.hpp"

#include "weather.hpp"

namespace {

class FixedClock final : public rfmap::IClock {
public:
    [[nodiscard]] rfmap::TimePoint now() const override { return rfmap::Clock::from_time_t(1792324800); }
};

class CannedSource final : public rfmap::IForecastSource {
public:
    rfmap::json response;
    rfmap::json fetch(double, double, const std::string&) override { return response; }
    std::optional<rfmap::TimePoint> runTime() override { return rfmap::Clock::from_time_t(1792303200); }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    rfmap::json doc = rfmap::json::parse(data, data + size, nullptr, false);
    if (doc.is_discarded()) return 0;

    static const FixedClock clock;
    CannedSource source;
    source.response = std::move(doc);
    rfmap::Config::Weather cfg;
    cfg.tiers = {{"hourly", ""}, {"long-range", "ecmwf_ifs025"}};
    rfmap::WeatherService weather(cfg, &source, clock);

    // 2026-10-18T12:00Z, the hour the seed responses cover.
    const rfmap::ForecastAt at = weather.at(50.45, 30.52, rfmap::Clock::from_time_t(1792324800));
    RFMAP_FUZZ_CHECK(at.point.is_object());
    (void)weather.horizon(50.45, 30.52);
    return 0;
}
