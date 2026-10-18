#pragma once

#include "common.hpp"
#include "config.hpp"

#include <rfcore/link_budget.hpp>
#include <rfext/refractivity.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rfmap {

/// Where forecasts come from. OpenMeteoSource in the server; tests inject fixed responses.
class IForecastSource {
public:
    virtual ~IForecastSource() = default;
    /// Raw Open-Meteo /v1/forecast response (timeformat=unixtime) for the point and model set.
    virtual json fetch(double lat, double lon, const std::string& models) = 0;
    /// Initialisation time of the run the instance holds, if it can tell.
    virtual std::optional<TimePoint> runTime() = 0;
};

/// A self-hosted Open-Meteo instance.
class OpenMeteoSource final : public IForecastSource {
public:
    explicit OpenMeteoSource(const Config::Weather& cfg) : cfg_(cfg) {}
    json fetch(double lat, double lon, const std::string& models) override;
    std::optional<TimePoint> runTime() override;

    /// The hourly variables requested: surface values plus the pressure levels the refraction
    /// column needs.
    static std::string hourlyVariables();
    static const std::vector<int>& pressureLevelsHpa();

private:
    Config::Weather cfg_;
};

/// The forecast for one place and hour, ready for the RF calculation.
struct ForecastAt {
    json point;                          ///< OpenAPI WeatherPoint
    std::vector<rfext::AirSample> column;  ///< surface first; empty when no forecast covers the time
    std::optional<double> rainMmPerHour;
    std::string sourceLabel;             ///< "Open-Meteo <models>, run ..., valid ..."
    std::vector<rfcore::UncertaintyNote> notes;
    [[nodiscard]] bool covered() const { return !column.empty(); }
};

class WeatherService {
public:
    WeatherService(const Config::Weather& cfg, IForecastSource* source, const IClock& clock);

    /// Picks the forecast hour nearest to `time`. Beyond the forecast: tier "none", a note
    /// saying how far the forecast reaches, and an empty column; nothing is extrapolated.
    ForecastAt at(double lat, double lon, TimePoint time);
    /// OpenAPI ForecastHorizon for a probe point (the forecast's reach is the same everywhere
    /// for one model set, apart from missing values).
    json horizon(double lat, double lon);

    /// Mean radius of curvature sqrt(M N) at a latitude: k for a point needs a radius but has no
    /// path azimuth.
    static double meanRadius(double latitudeDeg);
    /// Moist air density from the ideal gas law for dry air and water vapour.
    static double airDensity(const rfext::AirSample& air);
    /// Geometric height from geopotential height (WMO, Earth radius 6 356 766 m).
    static double geometricHeight(double geopotentialM);

    [[nodiscard]] bool configured() const { return source_ != nullptr; }

private:
    struct Cached {
        json response;
        TimePoint fetched;
    };
    json fetchCached(double lat, double lon, const Config::WeatherTier& tier);
    std::optional<TimePoint> runTimeCached();
    /// The forecast for hour `i` of a tier's response (`hourly` is its hourly block); nothing when
    /// its surface values are missing.
    std::optional<ForecastAt> forecastAtHour(const json& response, const json& hourly, std::size_t i,
                                             const Config::WeatherTier& tier, TimePoint time,
                                             const std::optional<TimePoint>& run, double lat) const;

    Config::Weather cfg_;
    IForecastSource* source_;
    const IClock& clock_;
    std::mutex mutex_;
    std::map<std::string, Cached> cache_;
    std::optional<std::pair<TimePoint, std::optional<TimePoint>>> run_;
};

}  // namespace rfmap
