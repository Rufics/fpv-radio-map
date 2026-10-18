#include "weather.hpp"

#include <rfcore/earth_model.hpp>

#include <httplib.h>

#include <cmath>
#include <sstream>

namespace rfmap {
namespace {

/// "http://host:port/path?q" → ("http://host:port", "/path?q").
std::pair<std::string, std::string> splitUrl(const std::string& url) {
    const auto scheme = url.find("://");
    const auto slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (slash == std::string::npos) return {url, "/"};
    return {url.substr(0, slash), url.substr(slash)};
}

std::string num(double v) { return json(v).dump(); }

json httpGetJson(const std::string& url, int timeoutMs) {
    const auto [origin, path] = splitUrl(url);
    httplib::Client client(origin);
    const auto secs = timeoutMs / 1000;
    const auto usecs = (timeoutMs % 1000) * 1000;
    client.set_connection_timeout(secs, usecs);
    client.set_read_timeout(secs, usecs);
    auto res = client.Get(path);
    if (!res) throw std::runtime_error("forecast service unreachable (" + httplib::to_string(res.error()) + ")");
    if (res->status != 200) {
        std::string reason;
        try {
            reason = json::parse(res->body).value("reason", "");
        } catch (const std::exception&) {
        }
        throw std::runtime_error("forecast service answered HTTP " + std::to_string(res->status) +
                                 (reason.empty() ? "" : ": " + reason));
    }
    return json::parse(res->body);
}

std::optional<double> valueAt(const json& hourly, const std::string& var, std::size_t i) {
    if (!hourly.contains(var)) return std::nullopt;
    const json& arr = hourly.at(var);
    if (!arr.is_array() || i >= arr.size() || !arr.at(i).is_number()) return std::nullopt;
    return arr.at(i).get<double>();
}

/// The last hour with the surface values the column needs, if any.
std::optional<TimePoint> lastSurfaceHour(const json& hourly) {
    const json& times = hourly.value("time", json::array());
    for (std::size_t i = times.size(); i-- > 0;) {
        if (valueAt(hourly, "temperature_2m", i).has_value() && valueAt(hourly, "surface_pressure", i).has_value()) {
            return Clock::from_time_t(times.at(i).get<std::time_t>());
        }
    }
    return std::nullopt;
}

/// Appends the pressure levels of hour `i` that lie above the ground; returns how many.
std::size_t addPressureLevels(std::vector<rfext::AirSample>& column, const json& hourly, std::size_t i,
                              double surface) {
    std::size_t levels = 0;
    for (const int level : OpenMeteoSource::pressureLevelsHpa()) {
        const std::string s = std::to_string(level) + "hPa";
        const auto t = valueAt(hourly, "temperature_" + s, i);
        const auto rh = valueAt(hourly, "relative_humidity_" + s, i);
        const auto gh = valueAt(hourly, "geopotential_height_" + s, i);
        if (!t.has_value() || !rh.has_value() || !gh.has_value()) continue;
        const double h = WeatherService::geometricHeight(*gh);
        // A level the model extrapolates below the ground is not air the path flies through.
        if (!(h > surface)) continue;
        column.emplace_back(rfext::Metres(h), *t, static_cast<double>(level), *rh);
        ++levels;
    }
    return levels;
}

}  // namespace

const std::vector<int>& OpenMeteoSource::pressureLevelsHpa() {
    static const std::vector<int> levels = {1000, 975, 950, 925, 900, 850};
    return levels;
}

std::string OpenMeteoSource::hourlyVariables() {
    std::string v = "temperature_2m,relative_humidity_2m,surface_pressure,precipitation,wind_speed_10m,wind_direction_10m";
    for (int l : pressureLevelsHpa()) {
        const std::string s = std::to_string(l) + "hPa";
        v += ",temperature_" + s + ",relative_humidity_" + s + ",geopotential_height_" + s;
    }
    return v;
}

json OpenMeteoSource::fetch(double lat, double lon, const std::string& models) {
    std::string url = cfg_.baseUrl + "/v1/forecast?latitude=" + num(lat) + "&longitude=" + num(lon) +
                      "&hourly=" + hourlyVariables() + "&timeformat=unixtime&timezone=GMT&wind_speed_unit=ms" +
                      "&past_days=1&forecast_days=" + std::to_string(cfg_.forecastDays);
    if (!models.empty()) url += "&models=" + models;
    return httpGetJson(url, cfg_.timeoutMs);
}

std::optional<TimePoint> OpenMeteoSource::runTime() {
    if (cfg_.metaUrl.empty()) return std::nullopt;
    const json meta = httpGetJson(cfg_.metaUrl, cfg_.timeoutMs);
    if (!meta.contains("last_run_initialisation_time")) return std::nullopt;
    return Clock::from_time_t(meta.at("last_run_initialisation_time").get<std::time_t>());
}

WeatherService::WeatherService(const Config::Weather& cfg, IForecastSource* source, const IClock& clock)
    : cfg_(cfg), source_(source), clock_(clock) {}

double WeatherService::meanRadius(double latitudeDeg) {
    const double m = rfcore::EarthModel::radiusOfCurvature(latitudeDeg, 0.0).value();
    const double n = rfcore::EarthModel::radiusOfCurvature(latitudeDeg, 90.0).value();
    return std::sqrt(m * n);
}

double WeatherService::airDensity(const rfext::AirSample& air) {
    constexpr double kR = 8.314462618;      // J/(mol K), exact (SI 2019)
    constexpr double kMd = 0.0289652;       // kg/mol, dry air
    constexpr double kMv = 0.018015268;     // kg/mol, water
    const double t = air.temperatureC + 273.15;
    const double e = air.relativeHumidity / 100.0 *
                     rfext::Refractivity::saturationVapourPressure(air.temperatureC, air.pressureHpa);
    const double pd = air.pressureHpa - e;
    return (pd * 100.0) * kMd / (kR * t) + (e * 100.0) * kMv / (kR * t);
}

double WeatherService::geometricHeight(double z) {
    constexpr double kRe = 6356766.0;
    return kRe * z / (kRe - z);
}

json WeatherService::fetchCached(double lat, double lon, const Config::WeatherTier& tier) {
    const std::string key = num(lat) + "," + num(lon) + "," + tier.models;
    const TimePoint now = clock_.now();
    {
        std::lock_guard lock(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end() && now - it->second.fetched < std::chrono::minutes(cfg_.cacheMinutes)) {
            return it->second.response;
        }
    }
    json r = source_->fetch(lat, lon, tier.models);
    std::lock_guard lock(mutex_);
    if (cache_.size() > 5000) cache_.clear();
    cache_[key] = {r, now};
    return r;
}

std::optional<TimePoint> WeatherService::runTimeCached() {
    const TimePoint now = clock_.now();
    {
        std::lock_guard lock(mutex_);
        if (run_ && now - run_->first < std::chrono::minutes(cfg_.cacheMinutes)) return run_->second;
    }
    std::optional<TimePoint> t;
    try {
        t = source_->runTime();
    } catch (const std::exception&) {
    }
    std::lock_guard lock(mutex_);
    run_ = std::make_pair(now, t);
    return t;
}

ForecastAt WeatherService::at(double lat, double lon, TimePoint time) {
    ForecastAt out;
    out.point = {{"tier", "none"}, {"model", "none"}};
    if (!source_) {
        out.notes.push_back({"Weather", "no forecast service is configured; standard atmosphere k = 4/3 used, no rain"});
        return out;
    }
    const auto run = runTimeCached();
    std::string lastError;
    std::optional<TimePoint> reach;
    for (const Config::WeatherTier& tier : cfg_.tiers) {
        json r;
        try {
            r = fetchCached(lat, lon, tier);
        } catch (const std::exception& e) {
            lastError = e.what();
            continue;
        }
        const json& hourly = r.value("hourly", json::object());
        const json& times = hourly.value("time", json::array());
        if (times.empty()) continue;
        if (const auto last = lastSurfaceHour(hourly); last.has_value() && (!reach.has_value() || *last > *reach)) reach = last;
        const auto t0 = Clock::from_time_t(times.front().get<std::time_t>());
        const double idx = std::round(std::chrono::duration<double>(time - t0).count() / 3600.0);
        if (idx < 0 || idx >= static_cast<double>(times.size())) continue;
        if (auto found = forecastAtHour(r, hourly, static_cast<std::size_t>(idx), tier, time, run, lat)) return *found;
    }
    out.point["requestedUtc"] = isoUtc(time);
    if (!lastError.empty() && !reach) {
        out.notes.push_back({"Weather", "forecast unavailable (" + lastError +
                                            "); standard atmosphere k = 4/3 used, no rain term"});
        out.point["error"] = lastError;
    } else {
        out.notes.push_back({"Weather", "the requested time " + isoUtc(time) + " is outside the forecast" +
                                            (reach ? " (it reaches " + isoUtc(*reach) + ")" : "") +
                                            "; standard atmosphere k = 4/3 used, no rain term"});
        if (reach) out.point["forecastUntilUtc"] = isoUtc(*reach);
    }
    if (run) out.point["runUtc"] = isoUtc(*run);
    return out;
}

std::optional<ForecastAt> WeatherService::forecastAtHour(const json& r, const json& hourly, std::size_t i,
                                                         const Config::WeatherTier& tier, TimePoint time,
                                                         const std::optional<TimePoint>& run, double lat) const {
    const auto t2 = valueAt(hourly, "temperature_2m", i);
    const auto rh2 = valueAt(hourly, "relative_humidity_2m", i);
    const auto ps = valueAt(hourly, "surface_pressure", i);
    if (!t2.has_value() || !rh2.has_value() || !ps.has_value() || !r.contains("elevation")) return std::nullopt;

    ForecastAt out;
    const TimePoint valid = Clock::from_time_t(hourly.value("time", json::array()).at(i).get<std::time_t>());
    const double surface = r.at("elevation").get<double>() + 2.0;
    out.column.push_back({rfext::Metres(surface), *t2, *ps, *rh2});
    const std::size_t levels = addPressureLevels(out.column, hourly, i, surface);
    out.rainMmPerHour = valueAt(hourly, "precipitation", i);

    const std::string models = tier.models.empty() ? "default models" : tier.models;
    const auto offsetMin = std::chrono::duration_cast<std::chrono::minutes>(time - valid).count();
    out.sourceLabel = "Open-Meteo " + models + ", valid " + isoUtc(valid) + (run ? ", run " + isoUtc(*run) : "");
    json& p = out.point;
    p = {{"model", "Open-Meteo " + models},
         {"validUtc", isoUtc(valid)},
         {"tier", tier.name},
         {"temperatureC", *t2},
         {"pressureHpa", *ps},
         {"relativeHumidity", *rh2},
         {"airDensityKgM3", airDensity(out.column.front())},
         {"surfaceElevationM", surface - 2.0},
         {"stale", false}};
    if (const auto ws = valueAt(hourly, "wind_speed_10m", i); ws.has_value()) p["windSpeedMps"] = *ws;
    if (const auto wd = valueAt(hourly, "wind_direction_10m", i); wd.has_value()) p["windFromDeg"] = *wd;
    if (out.rainMmPerHour.has_value()) p["precipitationMmPerHour"] = *out.rainMmPerHour;
    if (run) {
        p["runUtc"] = isoUtc(*run);
        p["stale"] = clock_.now() - *run > std::chrono::hours(cfg_.staleAfterHours);
    }
    if (const auto est = rfext::refractionForPath(out.column, rfext::Metres(surface))) {
        p["refractivityGradientPerKm"] = est->gradientPerKm;
        p["surfaceRefractivityN"] = est->nSurface;
        p["effectiveEarthFactor"] =
            rfcore::EarthModel::factorFromRefractivityGradient(est->gradientPerKm, rfcore::Metres(meanRadius(lat)));
    }
    out.notes.push_back({"Forecast", out.sourceLabel + (offsetMin != 0 ? " (" + std::to_string(offsetMin) +
                                                                               " min from the requested time)"
                                                                         : "")});
    out.notes.push_back({"Forecast air column",
                         "surface air is the 2 m temperature and humidity with the surface pressure; " +
                             std::to_string(levels) + " pressure levels above the ground; forecast values are "
                                                      "model output with their own error, not measurements"});
    if (levels == 0) {
        out.notes.push_back({"Refraction", "no forecast pressure level lies above the ground here; k cannot be "
                                           "derived and the standard atmosphere k = 4/3 is used"});
    }
    return out;
}

json WeatherService::horizon(double lat, double lon) {
    json out = json::object();
    if (!source_) return {{"configured", false}};
    if (const auto run = runTimeCached()) out["runUtc"] = isoUtc(*run);
    for (const Config::WeatherTier& tier : cfg_.tiers) {
        json r;
        try {
            r = fetchCached(lat, lon, tier);
        } catch (const std::exception& e) {
            out["error"] = e.what();
            continue;
        }
        if (const auto last = lastSurfaceHour(r.value("hourly", json::object()))) {
            out[tier.name == "long-range" ? "longRangeUntilUtc" : "hourlyUntilUtc"] = isoUtc(*last);
        }
    }
    return out;
}

}  // namespace rfmap
