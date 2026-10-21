#include "rfflightlog/story.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace rfflightlog {
namespace {

/// A disarm logged within this of an armed→disarmed state change is the same moment.
constexpr double kSameMomentS = 1.0;

/// One run of samples below a threshold.
struct LowRun {
    std::size_t from = 0;
    std::size_t to = 0;
    std::size_t lowest = 0;
};

/// Runs of `v` below `limit`, only where `keep(i)` holds, at least `minS` long.
template <typename Keep>
std::vector<LowRun> lowRuns(const std::vector<double>& t, const std::vector<double>& v, double limit, double minS,
                            Keep keep) {
    std::vector<LowRun> out;
    std::optional<LowRun> run;
    const auto close = [&](std::size_t end) {
        if (run && t[end] - t[run->from] >= minS) out.push_back(*run);
        run.reset();
    };
    for (std::size_t i = 0; i < v.size(); ++i) {
        const bool low = hasValue(v[i]) && v[i] < limit && keep(i);
        if (!low) {
            if (run) close(run->to);
            continue;
        }
        if (!run) run = LowRun{i, i, i};
        run->to = i;
        if (v[i] < v[run->lowest]) run->lowest = i;
    }
    if (run) close(run->to);
    return out;
}

std::string fixed(double v, int digits) { return std::format("{:.{}f}", v, digits); }

std::string distanceText(double m) { return m >= 1000.0 ? fixed(m / 1000.0, 2) + " km" : fixed(m, 0) + " m"; }

/// The largest value of a channel and its index.
std::optional<std::pair<std::size_t, double>> largest(const std::vector<double>* v) {
    if (v == nullptr) return std::nullopt;
    std::optional<std::pair<std::size_t, double>> best;
    for (std::size_t i = 0; i < v->size(); ++i) {
        if (hasValue((*v)[i]) && (!best || (*v)[i] > best->second)) best = std::make_pair(i, (*v)[i]);
    }
    return best;
}

std::optional<double> smallestValue(const std::vector<double>* v) {
    if (v == nullptr) return std::nullopt;
    std::optional<double> out;
    for (const double x : *v) {
        if (hasValue(x) && (!out || x < *out)) out = x;
    }
    return out;
}

std::optional<double> lastValue(const std::vector<double>* v) {
    if (v == nullptr) return std::nullopt;
    for (auto it = v->rbegin(); it != v->rend(); ++it) {
        if (hasValue(*it)) return *it;
    }
    return std::nullopt;
}

std::optional<double> firstValue(const std::vector<double>* v) {
    if (v == nullptr) return std::nullopt;
    for (const double x : *v) {
        if (hasValue(x)) return x;
    }
    return std::nullopt;
}

void putOptional(json& out, const char* key, const std::optional<double>& v) {
    if (v.has_value()) out[key] = std::round(*v * 1000.0) / 1000.0;
}

std::string failsafeText(const std::string& phase) {
    static const std::map<std::string, std::string, std::less<>> kText = {
        {"RX_LOSS_DETECTED", "receiver signal lost, failsafe started"},
        {"LANDING", "failsafe landing"},
        {"LANDED", "failsafe landed and disarmed"},
        {"RX_LOSS_MONITORING", "waiting for the signal to come back"},
        {"RX_LOSS_RECOVERED", "signal back, handing control to the pilot"},
        {"GPS_RESCUE", "GPS Rescue flying home"},
        {"AUTOPILOT", "autopilot took over"}};
    const auto it = kText.find(phase);
    return it == kText.end() ? phase : it->second;
}

}  // namespace

std::string ModeBandBuilder::bandOf(bool armed, const std::vector<std::string>& modes, const std::string& failsafe) {
    if (!armed) return "DISARMED";
    if (!failsafe.empty() && failsafe != "IDLE" && failsafe != "GPS_RESCUE") return "FAILSAFE";
    const auto has = [&modes](const char* name) { return std::ranges::find(modes, name) != modes.end(); };
    if (has("GPSRESCUE") || failsafe == "GPS_RESCUE") return "GPS_RESCUE";
    if (has("POSHOLD") || has("GPSHOLD")) return "POSHOLD";
    if (has("ALTHOLD") || has("BARO")) return "ALTHOLD";
    if (has("ANGLE")) return "ANGLE";
    if (has("HORIZON")) return "HORIZON";
    return "ACRO";
}

std::vector<ModeBand> ModeBandBuilder::build(const FlightLog& log) const {
    std::vector<ModeBand> out;
    bool armed = false;
    std::vector<std::string> modes;
    std::string failsafe;
    for (const StateChange& s : log.states) {
        armed = s.armed.value_or(armed);
        if (s.modes) modes = *s.modes;
        failsafe = s.failsafe.value_or(failsafe);
        const std::string band = bandOf(armed, modes, failsafe);
        if (!out.empty() && out.back().band == band) continue;
        if (!out.empty()) out.back().to = s.t;
        out.push_back({s.t, s.t, band});
    }
    if (!out.empty()) out.back().to = std::max(out.back().from, log.durationS);
    return out;
}

json ModeBandBuilder::toJson(const std::vector<ModeBand>& bands) {
    json out = json::array();
    for (const ModeBand& b : bands) out.push_back({{"from", b.from}, {"to", b.to}, {"band", b.band}});
    return out;
}

json FlightSummary::toJson() const {
    json out = {{"durationS", durationS}, {"armedS", std::round(armedS * 1000.0) / 1000.0}};
    if (cells > 0) out["cells"] = cells;
    putOptional(out, "distanceM", distanceM);
    putOptional(out, "maxHomeDistM", maxHomeDistM);
    putOptional(out, "maxAltM", maxAltM);
    putOptional(out, "maxSpeedMps", maxSpeedMps);
    putOptional(out, "maxCurrentA", maxCurrentA);
    putOptional(out, "usedMah", usedMah);
    putOptional(out, "usedWh", usedWh);
    putOptional(out, "startVoltageV", startVoltageV);
    putOptional(out, "minVoltageV", minVoltageV);
    putOptional(out, "minCellV", minCellV);
    putOptional(out, "minLinkPct", minLinkPct);
    if (minLinkPct.has_value()) out["linkChannel"] = linkChannel;
    if (disarmReason.has_value()) out["disarmReason"] = *disarmReason;
    return out;
}

std::string SummaryBuilder::linkChannel(const Timeline& timeline) {
    return timeline.samples.has("linkQualityPct") ? "linkQualityPct" : "rssiPct";
}

FlightSummary SummaryBuilder::build(const FlightLog& log, const Timeline& timeline,
                                    const std::vector<ModeBand>& bands) const {
    FlightSummary s;
    s.durationS = log.durationS;
    for (const ModeBand& b : bands) {
        if (b.band != "DISARMED") s.armedS += b.to - b.from;
    }
    s.cells = TimelineBuilder::cells(log);
    const SampledChannels& c = timeline.samples;
    s.distanceM = lastValue(c.channel("distanceM"));
    if (const auto v = largest(c.channel("homeDistM"))) s.maxHomeDistM = v->second;
    if (const auto v = largest(c.channel("altM"))) s.maxAltM = v->second;
    if (const auto v = largest(c.channel("speedMps"))) s.maxSpeedMps = v->second;
    if (log.channel("current")) {
        if (const auto v = largest(c.channel("currentA"))) s.maxCurrentA = v->second;
        s.usedMah = lastValue(c.channel("usedMah"));
        s.usedWh = lastValue(c.channel("usedWh"));
    }
    s.startVoltageV = firstValue(c.channel("voltageV"));
    s.minVoltageV = smallestValue(c.channel("voltageV"));
    s.minCellV = smallestValue(c.channel("cellV"));
    s.linkChannel = linkChannel(timeline);
    s.minLinkPct = smallestValue(c.channel(s.linkChannel));
    for (const LogEventEntry& e : log.events) {
        if (e.type == "disarm") s.disarmReason = e.reason;
    }
    return s;
}

json StoryEvent::toJson() const {
    json out = {{"t", std::round(t * 1000.0) / 1000.0}, {"type", type}, {"severity", severity}, {"title", title}};
    if (!detail.empty()) out["detail"] = detail;
    if (lat && lon) {
        out["lat"] = *lat;
        out["lon"] = *lon;
    }
    return out;
}

std::string StoryBuilder::disarmText(const std::string& reason) {
    static const std::map<std::string, std::string, std::less<>> kText = {
        {"ARMING_DISABLED", "arming was disabled by the flight controller"},
        {"FAILSAFE", "failsafe disarmed the craft"},
        {"THROTTLE_TIMEOUT", "throttle stayed low too long after arming"},
        {"STICKS", "disarmed with the sticks"},
        {"SWITCH", "disarmed with the arm switch"},
        {"CRASH_PROTECTION", "crash detection disarmed the craft"},
        {"RUNAWAY_TAKEOFF", "runaway takeoff prevention disarmed the craft"},
        {"GPS_RESCUE", "GPS Rescue disarmed the craft"},
        {"SERIAL_COMMAND", "disarmed by a serial (MSP) command"},
        {"LANDING", "disarmed after landing"},
        {"CRASHFLIP", "disarmed after crash flip"},
        {"LAUNCH_ABORT", "launch aborted"}};
    const auto it = kText.find(reason);
    return it == kText.end() ? "reason " + reason : it->second;
}

std::string StoryBuilder::bandTitle(const std::string& band) {
    static const std::map<std::string, std::string, std::less<>> kTitle = {
        {"DISARMED", "Disarmed"}, {"ACRO", "Acro"}, {"ANGLE", "Angle"}, {"HORIZON", "Horizon"},
        {"ALTHOLD", "Altitude hold"}, {"POSHOLD", "Position hold"}, {"GPS_RESCUE", "GPS Rescue"},
        {"FAILSAFE", "Failsafe"}};
    const auto it = kTitle.find(band);
    return it == kTitle.end() ? band : it->second;
}

void StoryBuilder::addStates(const FlightLog& log, std::vector<StoryEvent>& out) const {
    std::optional<bool> armed;
    std::string failsafe = "IDLE";
    bool signal = true;
    for (const StateChange& s : log.states) {
        if (s.armed && *s.armed != armed.value_or(false)) {
            if (*s.armed) out.push_back({s.t, "armed", "info", armed ? "Armed" : "Log starts armed", "", {}, {}});
            else out.push_back({s.t, "disarmed", "info", "Disarmed", "", {}, {}});
        }
        if (s.armed) armed = s.armed;
        if (s.failsafe && *s.failsafe != failsafe) {
            const bool idle = *s.failsafe == "IDLE";
            out.push_back({s.t, "failsafe", idle ? "info" : "blocked", idle ? "Failsafe over" : "Failsafe",
                           idle ? "" : failsafeText(*s.failsafe), {}, {}});
            failsafe = *s.failsafe;
        }
        if (s.rxSignal && *s.rxSignal != signal) {
            signal = *s.rxSignal;
            out.push_back({s.t, "rx-loss", signal ? "info" : "warn", signal ? "Receiver signal back" : "Receiver signal lost",
                           "", {}, {}});
        }
    }
}

void StoryBuilder::addBands(const std::vector<ModeBand>& bands, std::vector<StoryEvent>& out) const {
    for (std::size_t i = 1; i < bands.size(); ++i) {
        const ModeBand& b = bands[i];
        if (b.band == "DISARMED" || b.band == "FAILSAFE" || bands[i - 1].band == "DISARMED") continue;
        out.push_back({b.from, "mode", b.band == "GPS_RESCUE" ? "warn" : "info", "Mode: " + bandTitle(b.band),
                       "from " + bandTitle(bands[i - 1].band), {}, {}});
    }
}

void StoryBuilder::addLowLink(const Timeline& timeline, std::vector<StoryEvent>& out) const {
    const std::string name = SummaryBuilder::linkChannel(timeline);
    const std::vector<double>* v = timeline.samples.channel(name);
    if (v == nullptr) return;
    const std::vector<double>* armed = timeline.samples.channel("armed");
    const auto keep = [armed](std::size_t i) { return armed == nullptr || (*armed)[i] > 0.5; };
    const char* what = name == "linkQualityPct" ? "Link quality" : "RSSI";
    for (const LowRun& r : lowRuns(timeline.samples.t, *v, options_.lowLinkPct, options_.minLowS, keep)) {
        const double secs = timeline.samples.t[r.to] - timeline.samples.t[r.from];
        out.push_back({timeline.samples.t[r.from], "link-low", "warn", std::string("Weak link: ") + what + " low",
                       std::format("below {} % for {} s, lowest {} %", fixed(options_.lowLinkPct, 0), fixed(secs, 1),
                                   fixed((*v)[r.lowest], 0)),
                       {}, {}});
    }
}

void StoryBuilder::addLowCell(const FlightLog& log, const Timeline& timeline, std::vector<StoryEvent>& out) const {
    const std::vector<double>* cell = timeline.samples.channel("cellV");
    if (cell == nullptr) return;
    const double limit = options_.lowCellV.value_or(log.battery.cellWarnV.value_or(3.5));
    const auto runs = lowRuns(timeline.samples.t, *cell, limit, options_.minLowCellS, [](std::size_t) { return true; });
    if (runs.empty()) return;
    const LowRun& r = runs.front();
    out.push_back({timeline.samples.t[r.from], "low-cell", "warn", "Battery low",
                   std::format("cells under {} V for {} s", fixed(limit, 2), fixed(options_.minLowCellS, 0)), {}, {}});
}

void StoryBuilder::addGpsLoss(const Timeline& timeline, std::vector<StoryEvent>& out) const {
    const std::vector<double>* fix = timeline.samples.channel("gpsFix");
    if (fix == nullptr) return;
    for (std::size_t i = 1; i < fix->size(); ++i) {
        if ((*fix)[i - 1] > 0.5 && (*fix)[i] < 0.5) {
            out.push_back({timeline.samples.t[i], "gps-lost", "warn", "GPS fix lost", "", {}, {}});
        }
    }
}

void StoryBuilder::addRecords(const Timeline& timeline, std::vector<StoryEvent>& out) const {
    const SampledChannels& c = timeline.samples;
    if (const auto v = largest(c.channel("homeDistM")); v && v->second > 0.0) {
        out.push_back({c.t[v->first], "farthest", "info", "Farthest point", distanceText(v->second) + " from home", {}, {}});
    }
    if (const auto v = largest(c.channel("altM")); v && v->second > 0.0) {
        out.push_back({c.t[v->first], "highest", "info", "Highest point", fixed(v->second, 0) + " m above takeoff", {}, {}});
    }
    if (const auto v = largest(c.channel("speedMps")); v && v->second > 0.0) {
        out.push_back({c.t[v->first], "fastest", "info", "Top speed",
                       fixed(v->second * 3.6, 0) + " km/h over ground", {}, {}});
    }
}

void StoryBuilder::addLogEvents(const FlightLog& log, std::vector<StoryEvent>& out) const {
    for (const LogEventEntry& e : log.events) {
        if (e.type == "gap") {
            const bool corrupt = e.reason == "corrupt";
            out.push_back({e.t, "gap", "warn", "Gap in the log",
                           fixed(e.durationS, 2) + (corrupt ? " s of damaged data skipped" : " s with no data logged"), {}, {}});
        } else if (e.type == "logging-paused") {
            out.push_back({e.t, "paused", "info", "Logging paused", "for " + fixed(e.durationS, 1) + " s", {}, {}});
        } else if (e.type == "disarm") {
            const bool bad = e.reason == "FAILSAFE" || e.reason == "CRASH_PROTECTION" || e.reason == "RUNAWAY_TAKEOFF";
            // The disarm reason replaces the plain state change logged at the same moment.
            std::erase_if(out, [&e](const StoryEvent& s) { return s.type == "disarmed" && std::abs(s.t - e.t) <= kSameMomentS; });
            out.push_back({e.t, "disarmed", bad ? "blocked" : "info", "Disarmed", disarmText(e.reason), {}, {}});
        }
    }
}

void StoryBuilder::place(const Timeline& timeline, std::vector<StoryEvent>& events) {
    for (StoryEvent& e : events) {
        // Where the craft was last seen, when the GPS has no position at that very moment.
        for (std::size_t i = timeline.indexAt(e.t) + 1; i-- > 0;) {
            const double lat = timeline.at("lat", i);
            const double lon = timeline.at("lon", i);
            if (hasValue(lat) && hasValue(lon)) {
                e.lat = lat;
                e.lon = lon;
                break;
            }
        }
    }
}

std::vector<StoryEvent> StoryBuilder::build(const FlightLog& log, const Timeline& timeline,
                                            const std::vector<ModeBand>& bands) const {
    std::vector<StoryEvent> out;
    addStates(log, out);
    addBands(bands, out);
    addLowLink(timeline, out);
    addLowCell(log, timeline, out);
    addGpsLoss(timeline, out);
    addRecords(timeline, out);
    addLogEvents(log, out);
    const auto disarmedLater = [&log, &bands](const LogEventEntry& e) { return e.type == "disarm" && e.t >= bands.back().from; };
    if (!bands.empty() && bands.back().band != "DISARMED" && std::ranges::none_of(log.events, disarmedLater)) {
        out.push_back({log.durationS, "log-stops-armed", "warn", "Log stops while armed",
                       "No disarm was logged: the battery was unplugged, the craft crashed or the flash chip filled up.",
                       {}, {}});
    }
    std::ranges::stable_sort(out, {}, &StoryEvent::t);
    place(timeline, out);
    return out;
}

}  // namespace rfflightlog
