#include "weather.hpp"

#include "service_errors.hpp"

#include <rfcore/earth_model.hpp>

#include <httplib.h>

#include <cmath>
#include <format>
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
    if (!res) {
        throw ForecastUnavailableError(
            std::format("forecast service unreachable ({})", httplib::to_string(res.error())));
    }
    if (res->status != 200) {
        // The error says what failed, with the service's reason when its body gives one.
        const json body = json::parse(res->body, nullptr, false);
        std::string reason;
        if (body.is_object() && body.contains("reason") && body.at("reason").is_string()) {
            reason = ": " + body.at("reason").get<std::string>();
        }
        throw ForecastUnavailableError(std::format("forecast service answered HTTP {}{}", res->status, reason));
    }
    return json::parse(res->body);
}

std::optional<double> valueAt(const json& hourly, const std::string& var, std::size_t i) {
    if (!hourly.contains(var)) return std::nullopt;
    const json& arr = hourly.at(var);
    if (!arr.is_array() || i >= arr.size() || !arr.at(i).is_number()) return std::nullopt;
    return arr.at(i).get<double>();
}

/// A Unix time from the forecast service. Refuses what the clock cannot hold (it counts
/// nanoseconds in 64 bits) instead of overflowing.
TimePoint unixTime(const json& v) {
    constexpr std::int64_t kMin = -2208988800;  // 1900-01-01
    constexpr std::int64_t kMax = 7258118400;   // 2200-01-01
    if (!v.is_number_integer()) throw std::invalid_argument("forecast time is not a whole number of seconds");
    const auto s = v.get<std::int64_t>();
    if (v.is_number_unsigned() ? v.get<std::uint64_t>() > static_cast<std::uint64_t>(kMax) : s < kMin || s > kMax) {
        throw std::invalid_argument("forecast time is out of range");
    }
    return Clock::from_time_t(static_cast<std::time_t>(s));
}

/// Whole seconds from `from` to `to`. The clock counts nanoseconds in 64 bits, about 292 years, so
/// two times the forecast may hold (1900 to 2200) cannot be subtracted in its own units.
std::int64_t secondsBetween(TimePoint from, TimePoint to) {
    using std::chrono::floor;
    using std::chrono::seconds;
    return floor<seconds>(to).time_since_epoch().count() - floor<seconds>(from).time_since_epoch().count();
}

std::string unreadable(const std::exception& e) { return std::format("unreadable forecast response: {}", e.what()); }

json hourlyOf(const json& response) {
    return response.is_object() ? response.value("hourly", json::object()) : json::object();
}

json timesOf(const json& hourly) { return hourly.is_object() ? hourly.value("time", json::array()) : json::array(); }

/// The forecast hour nearest to `time`, when the response reaches it.
std::optional<std::size_t> hourIndex(const json& times, TimePoint time) {
    const double idx = std::round(static_cast<double>(secondsBetween(unixTime(times.front()), time)) / 3600.0);
    if (idx < 0 || idx >= static_cast<double>(times.size())) return std::nullopt;
    return static_cast<std::size_t>(idx);
}

struct SurfaceAir {
    double temperatureC;
    double relativeHumidity;
    double pressureHpa;
    double elevationM;
};

/// The 2 m air and the ground height at hour `i`, when the response has all of them.
std::optional<SurfaceAir> surfaceAir(const json& response, const json& hourly, std::size_t i) {
    const auto t2 = valueAt(hourly, "temperature_2m", i);
    const auto rh2 = valueAt(hourly, "relative_humidity_2m", i);
    const auto ps = valueAt(hourly, "surface_pressure", i);
    if (!t2.has_value() || !rh2.has_value() || !ps.has_value()) return std::nullopt;
    if (!response.contains("elevation") || !response.at("elevation").is_number()) return std::nullopt;
    return SurfaceAir{*t2, *rh2, *ps, response.at("elevation").get<double>()};
}

void setIfPresent(json& point, const char* key, const std::optional<double>& value) {
    if (value.has_value()) point[key] = *value;
}

void addForecastNotes(ForecastAt& out, std::int64_t offsetMin, std::size_t levels) {
    const std::string offset = offsetMin != 0 ? std::format(" ({} min from the requested time)", offsetMin) : "";
    out.notes.emplace_back("Forecast", std::format("{}{}", out.sourceLabel, offset));
    out.notes.emplace_back("Forecast air column",
                           std::format("surface air is the 2 m temperature and humidity with the surface pressure; "
                                       "{} pressure levels above the ground; forecast values are model output "
                                       "with their own error, not measurements",
                                       levels));
    if (levels == 0) {
        out.notes.emplace_back("Refraction", "no forecast pressure level lies above the ground here; k cannot be "
                                             "derived and the standard atmosphere k = 4/3 is used");
    }
}

}  // namespace

const std::vector<int>& OpenMeteoSource::pressureLevelsHpa() {
    static const std::vector levels = {1000, 975, 950, 925, 900, 850};
    return levels;
}

std::string OpenMeteoSource::hourlyVariables() {
    std::string v = "temperature_2m,relative_humidity_2m,surface_pressure,precipitation,wind_speed_10m,wind_direction_10m";
    for (int l : pressureLevelsHpa()) {
        v += std::format(",temperature_{0}hPa,relative_humidity_{0}hPa,geopotential_height_{0}hPa", l);
    }
    return v;
}

json OpenMeteoSource::fetch(double lat, double lon, const std::string& models) {
    std::string url = std::format("{}/v1/forecast?latitude={}&longitude={}&hourly={}&timeformat=unixtime&timezone=GMT"
                                  "&wind_speed_unit=ms&past_days=1&forecast_days={}",
                                  cfg_.baseUrl, num(lat), num(lon), hourlyVariables(), cfg_.forecastDays);
    if (!models.empty()) url += "&models=" + models;
    return httpGetJson(url, cfg_.timeoutMs);
}

std::optional<TimePoint> OpenMeteoSource::runTime() {
    if (cfg_.metaUrl.empty()) return std::nullopt;
    const json meta = httpGetJson(cfg_.metaUrl, cfg_.timeoutMs);
    if (!meta.contains("last_run_initialisation_time")) return std::nullopt;
    return unixTime(meta.at("last_run_initialisation_time"));
}

WeatherService::WeatherService(const Config::Weather& cfg, IForecastSource* source, const IClock& clock)
    : cfg_(cfg), source_(source), clock_(clock), cache_(std::chrono::minutes(cfg.cacheMinutes)) {}

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

WeatherService::ForecastCache::ForecastCache(std::chrono::minutes ttl)
    : ttl_(ttl), entries_(kMaxEntries, [](const Entry&) { return std::size_t{1}; }) {}

std::string WeatherService::ForecastCache::key(double lat, double lon, const std::string& models) {
    const auto cell = [](double deg) { return static_cast<long long>(std::llround(deg / kGridDeg)); };
    return std::format("{},{},{}", cell(lat), cell(lon), models);
}

std::optional<json> WeatherService::ForecastCache::lookup(const std::string& key, TimePoint now,
                                                          std::unique_lock<std::mutex>& lock) {
    fetched_.wait(lock, [this, &key] { return !inFlight_.contains(key); });
    if (const auto hit = entries_.get(key); hit && now - hit->fetched < ttl_) return hit->response;
    return std::nullopt;
}

json WeatherService::ForecastCache::get(double lat, double lon, const std::string& models, TimePoint now,
                                        const Fetch& fetch) {
    const std::string k = key(lat, lon, models);
    std::unique_lock lock(mutex_);
    if (const auto hit = lookup(k, now, lock)) return *hit;
    inFlight_.insert(k);
    lock.unlock();
    // Whatever the fetch does, the key must not stay in flight, or every later request waits.
    struct Done {
        ForecastCache& cache;
        const std::string& key;
        ~Done() {
            const std::lock_guard guard(cache.mutex_);
            cache.inFlight_.erase(key);
            cache.fetched_.notify_all();
        }
    } const done{*this, k};
    json r = fetch();
    entries_.put(k, std::make_shared<const Entry>(Entry{r, now}));
    return r;
}

json WeatherService::fetchCached(double lat, double lon, const Config::WeatherTier& tier) {
    return cache_.get(lat, lon, tier.models, clock_.now(),
                      [this, lat, lon, &tier] { return source_->fetch(lat, lon, tier.models); });
}

std::optional<json> WeatherService::fetchOrError(double lat, double lon, const Config::WeatherTier& tier,
                                                 std::string& error) {
    // A tier that cannot be fetched is skipped; the caller reports the error when no tier answers.
    try {
        return std::make_optional(fetchCached(lat, lon, tier));
    } catch (const json::exception& e) {
        error = e.what();
    } catch (const ForecastUnavailableError& e) {
        error = e.what();
    }
    return std::nullopt;
}

std::optional<TimePoint> WeatherService::runTimeCached() {
    const TimePoint now = clock_.now();
    {
        std::lock_guard lock(mutex_);
        if (run_ && now - run_->first < std::chrono::minutes(cfg_.cacheMinutes)) return run_->second;
    }
    const std::optional<TimePoint> t = tryRunTime();
    std::lock_guard lock(mutex_);
    run_ = std::make_pair(now, t);
    return t;
}

std::optional<TimePoint> WeatherService::tryRunTime() const {
    // A run time that cannot be had stays unknown; it is asked for again when the cache expires.
    try {
        return source_->runTime();
    } catch (const json::exception&) {
        return std::nullopt;
    } catch (const ForecastUnavailableError&) {
        return std::nullopt;
    } catch (const std::invalid_argument&) {
        return std::nullopt;  // a time the clock cannot hold
    }
}

std::optional<TimePoint> WeatherService::lastCoveredHour(const json& hourly, const json& times) {
    for (std::size_t i = times.size(); i-- > 0;) {
        if (valueAt(hourly, "temperature_2m", i).has_value() && valueAt(hourly, "surface_pressure", i).has_value()) {
            return unixTime(times.at(i));
        }
    }
    return std::nullopt;
}

std::size_t WeatherService::pressureLevels(const json& hourly, std::size_t i, double surface,
                                           std::vector<rfext::AirSample>& column) {
    std::size_t levels = 0;
    for (int level : OpenMeteoSource::pressureLevelsHpa()) {
        const auto t = valueAt(hourly, std::format("temperature_{}hPa", level), i);
        const auto rh = valueAt(hourly, std::format("relative_humidity_{}hPa", level), i);
        const auto gh = valueAt(hourly, std::format("geopotential_height_{}hPa", level), i);
        if (!t.has_value() || !rh.has_value() || !gh.has_value()) continue;
        const double h = geometricHeight(*gh);
        // A level the model extrapolates below the ground is not air the path flies through.
        if (!(h > surface)) continue;
        column.emplace_back(rfext::Metres(h), *t, static_cast<double>(level), *rh);
        ++levels;
    }
    return levels;
}

std::optional<ForecastAt> WeatherService::fromResponse(const json& r, const Config::WeatherTier& tier, double lat,
                                                       TimePoint time, const std::optional<TimePoint>& run,
                                                       std::optional<TimePoint>& reach) const {
    const json hourly = hourlyOf(r);
    const json times = timesOf(hourly);
    if (!times.is_array() || times.empty()) return std::nullopt;
    const auto index = hourIndex(times, time);
    // Last hour with the surface values the column needs.
    if (const auto last = lastCoveredHour(hourly, times); last && (!reach || *last > *reach)) reach = last;
    if (!index.has_value()) return std::nullopt;
    const std::size_t i = *index;
    const auto air = surfaceAir(r, hourly, i);
    if (!air.has_value()) return std::nullopt;

    ForecastAt out;
    const TimePoint valid = unixTime(times.at(i));
    const double surface = air->elevationM + 2.0;
    out.column.emplace_back(rfext::Metres(surface), air->temperatureC, air->pressureHpa, air->relativeHumidity);
    const std::size_t levels = pressureLevels(hourly, i, surface, out.column);
    out.rainMmPerHour = valueAt(hourly, "precipitation", i);

    const std::string models = tier.models.empty() ? "default models" : tier.models;
    out.sourceLabel = "Open-Meteo " + models + ", valid " + UtcTime::format(valid) + (run ? ", run " + UtcTime::format(*run) : "");
    json& p = out.point;
    p = {{"model", "Open-Meteo " + models},
         {"validUtc", UtcTime::format(valid)},
         {"tier", tier.name},
         {"temperatureC", air->temperatureC},
         {"pressureHpa", air->pressureHpa},
         {"relativeHumidity", air->relativeHumidity},
         {"airDensityKgM3", airDensity(out.column.front())},
         {"surfaceElevationM", air->elevationM},
         {"stale", false}};
    setIfPresent(p, "windSpeedMps", valueAt(hourly, "wind_speed_10m", i));
    setIfPresent(p, "windFromDeg", valueAt(hourly, "wind_direction_10m", i));
    setIfPresent(p, "precipitationMmPerHour", out.rainMmPerHour);
    if (run) {
        p["runUtc"] = UtcTime::format(*run);
        p["stale"] = secondsBetween(*run, clock_.now()) > std::int64_t{cfg_.staleAfterHours} * 3600;
    }
    if (const auto est = rfext::refractionForPath(out.column, rfext::Metres(surface))) {
        p["refractivityGradientPerKm"] = est->gradientPerKm;
        p["surfaceRefractivityN"] = est->nSurface;
        p["effectiveEarthFactor"] =
            rfcore::EarthModel::factorFromRefractivityGradient(est->gradientPerKm, rfcore::Metres(meanRadius(lat)));
    }
    addForecastNotes(out, secondsBetween(valid, time) / 60, levels);
    return out;
}

ForecastAt WeatherService::at(double lat, double lon, TimePoint time) {
    ForecastAt out;
    out.point = {{"tier", "none"}, {"model", "none"}};
    if (!source_) {
        out.notes.emplace_back("Weather",
                               "no forecast service is configured; standard atmosphere k = 4/3 used, no rain");
        return out;
    }
    const auto run = runTimeCached();
    std::string lastError;
    std::optional<TimePoint> reach;
    for (const Config::WeatherTier& tier : cfg_.tiers) {
        const std::optional<json> r = fetchOrError(lat, lon, tier, lastError);
        if (!r.has_value()) continue;
        try {
            if (auto found = fromResponse(*r, tier, lat, time, run, reach)) return *found;
        } catch (const json::exception& e) {
            // A response that does not read as a forecast is a failed tier, like an unreachable service.
            lastError = unreadable(e);
        } catch (const std::invalid_argument& e) {
            lastError = unreadable(e);  // a time the clock cannot hold
        }
    }
    out.point["requestedUtc"] = UtcTime::format(time);
    if (!lastError.empty() && !reach) {
        out.notes.emplace_back("Weather", std::format("forecast unavailable ({}); standard atmosphere k = 4/3 used, "
                                                      "no rain term",
                                                      lastError));
        out.point["error"] = lastError;
    } else {
        const std::string reaches = reach ? std::format(" (it reaches {})", UtcTime::format(*reach)) : "";
        out.notes.emplace_back("Weather", std::format("the requested time {} is outside the forecast{}; standard "
                                                      "atmosphere k = 4/3 used, no rain term",
                                                      UtcTime::format(time), reaches));
        if (reach) out.point["forecastUntilUtc"] = UtcTime::format(*reach);
    }
    if (run) out.point["runUtc"] = UtcTime::format(*run);
    return out;
}

json WeatherService::horizon(double lat, double lon) {
    json out = json::object();
    if (!source_) return {{"configured", false}};
    if (const auto run = runTimeCached()) out["runUtc"] = UtcTime::format(*run);
    for (const Config::WeatherTier& tier : cfg_.tiers) {
        std::string error;
        const std::optional<json> r = fetchOrError(lat, lon, tier, error);
        if (!r.has_value()) {
            out["error"] = error;
            continue;
        }
        try {
            const json hourly = hourlyOf(*r);
            const json times = timesOf(hourly);
            if (!times.is_array()) continue;
            if (const auto last = lastCoveredHour(hourly, times)) {
                out[tier.name == "long-range" ? "longRangeUntilUtc" : "hourlyUntilUtc"] = UtcTime::format(*last);
            }
        } catch (const json::exception& e) {
            out["error"] = unreadable(e);
        } catch (const std::invalid_argument& e) {
            out["error"] = unreadable(e);  // a time the clock cannot hold
        }
    }
    return out;
}

}  // namespace rfmap
