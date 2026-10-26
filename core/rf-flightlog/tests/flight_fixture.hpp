#pragma once

// A normalized flight log (flightlog/v1) built in code, so a test states the flight and checks
// what the processor makes of it: 60 s, armed from 2 s to 50 s, flying east at 10 m/s.

#include <rfflightlog/flight_log.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace rfflightlog::test {

class FlightFixture {
public:
    static constexpr double kHomeLat = 50.0;
    static constexpr double kHomeLon = 30.0;
    /// Metres per degree of longitude at 50° N, near enough for a fixture.
    static constexpr double kMetresPerDegLon = 71'696.0;

    double durationS = 60.0;
    double seriesHz = 50.0;
    double gpsHz = 10.0;
    double armS = 2.0;
    double disarmS = 50.0;
    double acroS = 20.0;
    double speedMps = 10.0;
    double currentA = 10.0;
    bool withCurrent = true;
    bool withGps = true;
    bool withLinkQuality = false;
    /// RSSI drops to `lowRssiPct` between these instants.
    double lowFromS = 30.0;
    double lowToS = 33.0;
    double lowRssiPct = 20.0;
    std::string disarmReason = "SWITCH";

    [[nodiscard]] double voltageAt(double t) const { return 16.8 - 1.8 * t / durationS; }
    [[nodiscard]] double eastAt(double t) const {
        const double flying = std::clamp(t, armS, disarmS) - armS;
        return flying * speedMps;
    }

    [[nodiscard]] json build() const {
        json log = {{"index", 0},
                    {"status", "ok"},
                    {"byteRange", {0, 1000}},
                    {"firmware", {{"name", "Betaflight"}, {"version", "4.5.1"}, {"supported", true}}},
                    {"craft", "Fixture"},
                    {"startedAt", "2024-09-22T19:43:32.234+00:00"},
                    {"durationS", durationS},
                    {"channels", channels()},
                    {"battery", {{"cellMinV", 3.3}, {"cellWarnV", 3.5}, {"cellMaxV", 4.3}}},
                    {"series", series()},
                    {"homes", json::array({{{"t", 0.0}, {"lat", kHomeLat}, {"lon", kHomeLon}}})},
                    {"states", states()},
                    {"events", events()},
                    {"notes", json::array()},
                    {"stats", {{"frames", json::object()}, {"corruptFrames", 0}}}};
        if (withGps) log["gps"] = gps();
        return {{"schema", "flightlog/v1"}, {"source", {{"decoder", "fixture"}}}, {"logs", json::array({log})}};
    }

private:
    [[nodiscard]] json channels() const {
        return {{"gps", withGps},          {"voltage", true},   {"current", withCurrent},
                {"baroAltitude", false},   {"rssi", true},      {"linkQuality", withLinkQuality},
                {"rssiDbm", false},        {"throttle", true},  {"modes", true}};
    }

    [[nodiscard]] json series() const {
        json t = json::array();
        json v = json::array();
        json a = json::array();
        json rssi = json::array();
        json lq = json::array();
        const auto n = static_cast<int>(durationS * seriesHz);
        for (int i = 0; i <= n; ++i) {
            const double s = i / seriesHz;
            t.push_back(s);
            v.push_back(voltageAt(s));
            a.push_back(currentA);
            const double pct = s >= lowFromS && s < lowToS ? lowRssiPct : 90.0;
            rssi.push_back(pct);
            lq.push_back(pct);
        }
        json out = {{"t", t}, {"voltageV", v}, {"rssiPct", rssi}};
        if (withCurrent) out["currentA"] = a;
        if (withLinkQuality) out["linkQualityPct"] = lq;
        return out;
    }

    [[nodiscard]] json gps() const {
        json out = {{"t", json::array()},   {"lat", json::array()},  {"lon", json::array()},  {"altM", json::array()},
                    {"speedMps", json::array()}, {"sats", json::array()}, {"fix", json::array()}};
        const auto n = static_cast<int>(durationS * gpsHz);
        for (int i = 0; i <= n; ++i) {
            const double s = i / gpsHz;
            const bool flying = s > armS && s < disarmS;
            out["t"].push_back(s);
            out["lat"].push_back(kHomeLat);
            out["lon"].push_back(kHomeLon + eastAt(s) / kMetresPerDegLon);
            out["altM"].push_back(120.0 + (flying ? 30.0 : 0.0));
            out["speedMps"].push_back(flying ? speedMps : 0.0);
            out["sats"].push_back(14);
            out["fix"].push_back(true);
        }
        return out;
    }

    [[nodiscard]] json states() const {
        const auto state = [](double t, bool armed, json modes) {
            return json{{"t", t},          {"armed", armed},     {"modes", std::move(modes)}, {"failsafe", "IDLE"},
                        {"rxSignal", true}, {"rxValid", true},   {"gpsFix", true},            {"gpsHomeSet", true}};
        };
        return json::array({state(0.0, false, json::array({"ANGLE"})), state(armS, true, json::array({"ANGLE"})),
                            state(acroS, true, json::array()), state(disarmS, false, json::array())});
    }

    [[nodiscard]] json events() const {
        json out = json::array({{{"t", 0.0}, {"type", "sync-beep"}}});
        if (!disarmReason.empty()) out.push_back({{"t", disarmS + 0.02}, {"type", "disarm"}, {"reason", disarmReason}});
        out.push_back({{"t", durationS}, {"type", "log-end"}});
        return out;
    }
};

}  // namespace rfflightlog::test
