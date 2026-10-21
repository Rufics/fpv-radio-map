#pragma once

// One flight of the normalized flight log (schemas/flightlog.schema.json), read into typed
// arrays. Every decoder (Betaflight Blackbox today, DJI later) answers in that format, so
// everything after this point is the same for every source.

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfflightlog {

using json = nlohmann::json;

/// A missing sample (JSON null).
inline constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();
inline bool hasValue(double v) { return !std::isnan(v); }

/// Samples on one time base; channels hold NaN where the log has no value.
class SampledChannels {
public:
    std::vector<double> t;

    [[nodiscard]] const std::vector<double>* channel(const std::string& name) const;
    [[nodiscard]] bool has(const std::string& name) const { return channel(name) != nullptr; }
    void set(const std::string& name, std::vector<double> values) { channels_[name] = std::move(values); }
    [[nodiscard]] const std::map<std::string, std::vector<double>>& all() const { return channels_; }

private:
    std::map<std::string, std::vector<double>> channels_;
};

struct GpsFix {
    std::vector<double> t;
    std::vector<double> lat;
    std::vector<double> lon;
    std::vector<double> altM;      ///< above mean sea level
    std::vector<double> speedMps;
    std::vector<double> sats;
    std::vector<bool> fix;
    [[nodiscard]] std::size_t size() const { return t.size(); }
    [[nodiscard]] bool usable(std::size_t i) const { return fix[i] && hasValue(lat[i]) && hasValue(lon[i]); }
};

struct HomePoint {
    double t = 0.0;
    double lat = 0.0;
    double lon = 0.0;
    std::optional<double> altM;
};

struct StateChange {
    double t = 0.0;
    std::optional<bool> armed;
    std::optional<std::vector<std::string>> modes;
    std::optional<std::string> failsafe;
    std::optional<bool> rxSignal;
    std::optional<bool> gpsFix;
};

struct LogEventEntry {
    double t = 0.0;
    std::string type;
    std::string reason;
    double durationS = 0.0;
};

struct BatteryLimits {
    std::optional<double> cellMinV;
    std::optional<double> cellWarnV;
    std::optional<double> cellMaxV;
};

class FlightLog {
public:
    /// One entry of `logs`, already checked against the schema.
    static FlightLog fromJson(const json& entry);

    int index = 0;
    std::string status;
    double durationS = 0.0;
    json firmware = json::object();
    json channels = json::object();
    json notes = json::array();
    std::optional<std::string> craft;
    std::optional<std::string> startedAt;
    std::optional<std::string> radioProtocol;
    BatteryLimits battery;
    SampledChannels series;
    GpsFix gps;
    std::vector<HomePoint> homes;
    std::vector<StateChange> states;
    std::vector<LogEventEntry> events;

    [[nodiscard]] bool channel(const std::string& name) const { return channels.value(name, false); }

private:
    static std::vector<double> numbers(const json& array);
    void readSeries(const json& s);
    void readGps(const json& g);
    void readStates(const json& list);
};

}  // namespace rfflightlog
