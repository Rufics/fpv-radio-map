#include "rfflightlog/flight_log.hpp"

namespace rfflightlog {

const std::vector<double>* SampledChannels::channel(std::string_view name) const {
    const auto it = byName.find(name);
    return it == byName.end() ? nullptr : &it->second;
}

std::vector<double> FlightLog::numbers(const json& array) {
    std::vector<double> out;
    out.reserve(array.size());
    for (const json& v : array) out.push_back(v.is_number() ? v.get<double>() : kNoValue);
    return out;
}

void FlightLog::readSeries(const json& s) {
    series.t = numbers(s.at("t"));
    for (const auto& [name, values] : s.items()) {
        if (name == "t") continue;
        std::vector<double> v = numbers(values);
        v.resize(series.t.size(), kNoValue);
        series.set(name, std::move(v));
    }
}

void FlightLog::readGps(const json& g) {
    gps.t = numbers(g.at("t"));
    const std::size_t n = gps.t.size();
    const auto column = [&g, n](const char* name) {
        std::vector<double> v = g.contains(name) ? numbers(g.at(name)) : std::vector<double>();
        v.resize(n, kNoValue);
        return v;
    };
    gps.lat = column("lat");
    gps.lon = column("lon");
    gps.altM = column("altM");
    gps.speedMps = column("speedMps");
    gps.sats = column("sats");
    gps.fix.assign(n, false);
    const json& fix = g.at("fix");
    for (std::size_t i = 0; i < n && i < fix.size(); ++i) gps.fix[i] = fix.at(i).get<bool>();
}

void FlightLog::readStates(const json& list) {
    for (const json& s : list) {
        StateChange c;
        c.t = s.at("t").get<double>();
        if (s.contains("armed")) c.armed = s.at("armed").get<bool>();
        if (s.contains("modes")) c.modes = s.at("modes").get<std::vector<std::string>>();
        if (s.contains("failsafe")) c.failsafe = s.at("failsafe").get<std::string>();
        if (s.contains("rxSignal")) c.rxSignal = s.at("rxSignal").get<bool>();
        if (s.contains("gpsFix")) c.gpsFix = s.at("gpsFix").get<bool>();
        states.push_back(std::move(c));
    }
}

FlightLog FlightLog::fromJson(const json& entry) {
    FlightLog log;
    log.index = entry.at("index").get<int>();
    log.status = entry.at("status").get<std::string>();
    log.durationS = entry.value("durationS", 0.0);
    log.firmware = entry.at("firmware");
    log.channels = entry.at("channels");
    log.notes = entry.at("notes");
    if (entry.contains("craft")) log.craft = entry.at("craft").get<std::string>();
    if (entry.contains("startedAt")) log.startedAt = entry.at("startedAt").get<std::string>();
    if (entry.contains("radio") && entry.at("radio").contains("protocol")) {
        log.radioProtocol = entry.at("radio").at("protocol").get<std::string>();
    }
    if (entry.contains("battery")) {
        const json& b = entry.at("battery");
        if (b.contains("cellMinV")) log.battery.cellMinV = b.at("cellMinV").get<double>();
        if (b.contains("cellWarnV")) log.battery.cellWarnV = b.at("cellWarnV").get<double>();
        if (b.contains("cellMaxV")) log.battery.cellMaxV = b.at("cellMaxV").get<double>();
    }
    if (entry.contains("series")) log.readSeries(entry.at("series"));
    if (entry.contains("gps")) log.readGps(entry.at("gps"));
    for (const json& h : entry.value("homes", json::array())) {
        HomePoint home{h.at("t").get<double>(), h.at("lat").get<double>(), h.at("lon").get<double>(), std::nullopt};
        if (h.contains("altM")) home.altM = h.at("altM").get<double>();
        log.homes.push_back(home);
    }
    log.readStates(entry.value("states", json::array()));
    for (const json& e : entry.value("events", json::array())) {
        log.events.emplace_back(e.at("t").get<double>(), e.at("type").get<std::string>(), e.value("reason", std::string()),
                                e.value("durationS", 0.0));
    }
    return log;
}

}  // namespace rfflightlog
