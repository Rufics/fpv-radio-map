#include "rfflightlog/timeline.hpp"

#include <rfcore/geodesy.hpp>

#include <algorithm>
#include <cmath>

namespace rfflightlog {
namespace {

/// Above this a long log is sampled more coarsely instead of growing without bound.
constexpr std::size_t kMaxSamples = 200'000;
/// GPS speed below which the craft is taken as standing (position noise is not distance).
constexpr double kStandingMps = 0.5;
/// A pack is read at the start of the log, before the voltage sags under load.
constexpr double kCellWindowS = 5.0;
/// More series cells than any flight controller reads: the voltage or the cell maximum is not real.
constexpr double kMaxCells = 16.0;

/// The last value at or before each instant, if it is no older than `maxAgeS`.
std::vector<double> holdLast(const std::vector<double>& t, const std::vector<double>& v, const std::vector<double>& at,
                             double maxAgeS) {
    std::vector out(at.size(), kNoValue);
    std::size_t i = 0;
    for (std::size_t k = 0; k < at.size(); ++k) {
        while (i + 1 < t.size() && t[i + 1] <= at[k]) ++i;
        if (t.empty() || t[i] > at[k]) continue;
        if (at[k] - t[i] <= maxAgeS) out[k] = v[i];
    }
    return out;
}

json numbersJson(const std::vector<double>& v) {
    json out = json::array();
    for (const double x : v) {
        if (hasValue(x)) out.push_back(std::round(x * 1e7) / 1e7);
        else out.push_back(nullptr);
    }
    return out;
}

/// The usable GPS fixes only, as plain columns.
struct Fixes {
    std::vector<double> t;
    std::vector<double> lat;
    std::vector<double> lon;
    std::vector<double> altM;
    std::vector<double> speedMps;

    explicit Fixes(const GpsFix& gps) {
        for (std::size_t i = 0; i < gps.size(); ++i) {
            if (!gps.usable(i)) continue;
            t.push_back(gps.t[i]);
            lat.push_back(gps.lat[i]);
            lon.push_back(gps.lon[i]);
            altM.push_back(gps.altM[i]);
            speedMps.push_back(gps.speedMps[i]);
        }
    }

    /// Geodesic distance flown up to each fix, leaving out jitter while standing still.
    [[nodiscard]] std::vector<double> distanceFlown(double maxGapS) const {
        const rfcore::Geodesy geo;
        std::vector out(t.size(), 0.0);
        for (std::size_t i = 1; i < t.size(); ++i) {
            out[i] = out[i - 1];
            if (const bool moving = !(speedMps[i] < kStandingMps && speedMps[i - 1] < kStandingMps);
                !moving || t[i] - t[i - 1] > maxGapS) {
                continue;
            }
            out[i] += geo.inverse({lat[i - 1], lon[i - 1]}, {lat[i], lon[i]}).distance.value();
        }
        return out;
    }
};

/// Fills each missing value with the one before it (running totals over a gap).
void carryForward(std::vector<double>& v) {
    double last = kNoValue;
    for (double& x : v) {
        if (hasValue(x)) last = x;
        else x = last;
    }
}

}  // namespace

double Timeline::at(const std::string& name, std::size_t i) const {
    const std::vector<double>* c = samples.channel(name);
    return c == nullptr || i >= c->size() ? kNoValue : (*c)[i];
}

std::size_t Timeline::indexAt(double t) const {
    if (size() == 0) return 0;
    const double i = std::round((t - samples.t.front()) * hz);
    if (!(i > 0.0)) return 0;
    return static_cast<std::size_t>(std::min(i, static_cast<double>(size() - 1)));
}

json Timeline::toJson() const {
    json channels = json::object();
    for (const auto& [name, values] : samples.all()) channels[name] = numbersJson(values);
    return {{"hz", hz}, {"t", numbersJson(samples.t)}, {"channels", channels}};
}

double Resampler::between(const std::vector<double>& t, const std::vector<double>& v, std::size_t i, double x) const {
    if (t[i] == x) return v[i];
    if (i + 1 >= t.size() || t[i + 1] - t[i] > maxGapS_) return kNoValue;
    if (!hasValue(v[i]) || !hasValue(v[i + 1])) return kNoValue;
    const double f = (x - t[i]) / (t[i + 1] - t[i]);
    return v[i] + (v[i + 1] - v[i]) * f;
}

std::vector<double> Resampler::resample(const std::vector<double>& t, const std::vector<double>& v,
                                        const std::vector<double>& at) const {
    std::vector out(at.size(), kNoValue);
    std::size_t i = 0;
    for (std::size_t k = 0; k < at.size(); ++k) {
        while (i + 1 < t.size() && t[i + 1] <= at[k]) ++i;
        if (t.empty() || t[i] > at[k]) continue;
        out[k] = between(t, v, i, at[k]);
    }
    return out;
}

EnergyMeter::Totals EnergyMeter::integrate(const std::vector<double>& t, const std::vector<double>& voltageV,
                                           const std::vector<double>& currentA) const {
    Totals out{std::vector<double>(t.size(), 0.0), std::vector<double>(t.size(), 0.0)};
    for (std::size_t i = 1; i < t.size(); ++i) {
        out.usedMah[i] = out.usedMah[i - 1];
        out.usedWh[i] = out.usedWh[i - 1];
        const double dt = t[i] - t[i - 1];
        if (dt <= 0.0 || dt > maxGapS_ || !hasValue(currentA[i]) || !hasValue(currentA[i - 1])) continue;
        const double amps = (currentA[i] + currentA[i - 1]) / 2.0;
        out.usedMah[i] += amps * dt / 3.6;
        if (hasValue(voltageV[i]) && hasValue(voltageV[i - 1])) {
            out.usedWh[i] += (voltageV[i] * currentA[i] + voltageV[i - 1] * currentA[i - 1]) / 2.0 * dt / 3600.0;
        }
    }
    return out;
}

int CellCounter::count(const std::vector<double>& t, const std::vector<double>& voltageV) const {
    double top = 0.0;
    for (std::size_t i = 0; i < t.size() && i < voltageV.size(); ++i) {
        if (t[i] - t.front() > kCellWindowS) break;
        if (hasValue(voltageV[i])) top = std::max(top, voltageV[i]);
    }
    if (!(top > 0.5) || !(cellMaxV_ > 0.0)) return 0;
    const double full = std::floor(top / cellMaxV_);
    if (!(full < kMaxCells)) return 0;
    return static_cast<int>(full) + 1;
}

int TimelineBuilder::cells(const FlightLog& log) {
    const std::vector<double>* v = log.series.channel("voltageV");
    if (v == nullptr) return 0;
    return CellCounter(log.battery.cellMaxV.value_or(4.3)).count(log.series.t, *v);
}

std::vector<double> TimelineBuilder::instants(const FlightLog& log) const {
    double end = log.durationS;
    if (!log.series.t.empty()) end = std::max(end, log.series.t.back());
    if (!log.gps.t.empty()) end = std::max(end, log.gps.t.back());
    if (!(end > 0.0)) end = 0.0;
    const double wanted = std::floor(end * options_.hz) + 1.0;
    const bool coarse = !(wanted <= static_cast<double>(kMaxSamples));
    std::vector<double> t(coarse ? kMaxSamples : static_cast<std::size_t>(wanted));
    const double step = coarse ? end / static_cast<double>(kMaxSamples - 1) : 1.0 / options_.hz;
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = static_cast<double>(i) * step;
    return t;
}

void TimelineBuilder::addSeries(const FlightLog& log, Timeline& out) const {
    const Resampler r(options_.maxGapS);
    for (const auto& [name, values] : log.series.all()) {
        out.samples.set(name, r.resample(log.series.t, values, out.samples.t));
    }
    const int n = cells(log);
    const std::vector<double>* v = out.samples.channel("voltageV");
    if (n <= 0 || v == nullptr) return;
    std::vector<double> cell(*v);
    for (double& x : cell) x /= n;
    out.samples.set("cellV", std::move(cell));
}

void TimelineBuilder::addEnergy(const FlightLog& log, Timeline& out) const {
    const std::vector<double>* v = log.series.channel("voltageV");
    const std::vector<double>* a = log.series.channel("currentA");
    if (!log.channel("current") || v == nullptr || a == nullptr) return;
    const EnergyMeter::Totals totals = EnergyMeter(options_.maxGapS).integrate(log.series.t, *v, *a);
    const double forever = std::numeric_limits<double>::infinity();
    std::vector<double> mah = holdLast(log.series.t, totals.usedMah, out.samples.t, forever);
    std::vector<double> wh = holdLast(log.series.t, totals.usedWh, out.samples.t, forever);
    carryForward(mah);
    carryForward(wh);
    out.samples.set("usedMah", std::move(mah));
    out.samples.set("usedWh", std::move(wh));
}

void TimelineBuilder::addGps(const FlightLog& log, Timeline& out) const {
    if (log.gps.size() == 0) return;
    const std::vector<double>& at = out.samples.t;
    std::vector<double> sats = holdLast(log.gps.t, log.gps.sats, at, options_.maxGpsGapS);
    std::vector<double> fix(log.gps.size());
    for (std::size_t i = 0; i < fix.size(); ++i) fix[i] = log.gps.fix[i] ? 1.0 : 0.0;
    out.samples.set("sats", std::move(sats));
    out.samples.set("gpsFix", holdLast(log.gps.t, fix, at, options_.maxGpsGapS));

    const Fixes fixes(log.gps);
    if (fixes.t.empty()) return;
    const Resampler r(options_.maxGpsGapS);
    std::vector<double> lat = r.resample(fixes.t, fixes.lat, at);
    std::vector<double> lon = r.resample(fixes.t, fixes.lon, at);
    std::vector<double> distance = holdLast(fixes.t, fixes.distanceFlown(options_.maxGpsGapS), at,
                                            std::numeric_limits<double>::infinity());
    carryForward(distance);
    out.samples.set("altMslM", r.resample(fixes.t, fixes.altM, at));
    out.samples.set("speedMps", r.resample(fixes.t, fixes.speedMps, at));
    out.samples.set("distanceM", std::move(distance));

    const rfcore::Geodesy geo;
    std::vector home(at.size(), kNoValue);
    std::size_t h = 0;
    for (std::size_t k = 0; k < at.size(); ++k) {
        while (h + 1 < log.homes.size() && log.homes[h + 1].t <= at[k]) ++h;
        const rfcore::GeoPoint base = log.homes.empty() ? rfcore::GeoPoint{fixes.lat.front(), fixes.lon.front()}
                                                        : rfcore::GeoPoint{log.homes[h].lat, log.homes[h].lon};
        if (hasValue(lat[k]) && hasValue(lon[k])) home[k] = geo.inverse(base, {lat[k], lon[k]}).distance.value();
    }
    out.samples.set("homeDistM", std::move(home));
    out.samples.set("lat", std::move(lat));
    out.samples.set("lon", std::move(lon));
}

void TimelineBuilder::addArmed(const FlightLog& log, Timeline& out) {
    if (log.states.empty()) return;
    std::vector<double> t;
    std::vector<double> armed;
    for (const StateChange& s : log.states) {
        if (!s.armed.has_value()) continue;
        t.push_back(s.t);
        armed.push_back(*s.armed ? 1.0 : 0.0);
    }
    if (t.empty()) return;
    std::vector<double> v = holdLast(t, armed, out.samples.t, std::numeric_limits<double>::infinity());
    for (double& x : v) {
        if (!hasValue(x)) x = armed.front();
    }
    out.samples.set("armed", std::move(v));
}

void TimelineBuilder::addAltitude(const FlightLog& log, Timeline& out) {
    if (const std::vector<double>* baro = out.samples.channel("baroAltM"); baro != nullptr) {
        out.samples.set("altM", *baro);
        return;
    }
    const std::vector<double>* msl = out.samples.channel("altMslM");
    if (msl == nullptr) return;
    double base = kNoValue;
    if (!log.homes.empty()) base = log.homes.front().altM.value_or(kNoValue);
    for (std::size_t i = 0; !hasValue(base) && i < log.gps.size(); ++i) {
        if (log.gps.usable(i)) base = log.gps.altM[i];
    }
    std::vector<double> alt(*msl);
    for (double& x : alt) x -= base;
    out.samples.set("altM", std::move(alt));
}

void TimelineBuilder::addClimb(Timeline& out) {
    const std::vector<double>* alt = out.samples.channel("altM");
    if (alt == nullptr) return;
    const auto k = static_cast<std::size_t>(std::max(1.0, std::round(out.hz / 2.0)));
    std::vector climb(alt->size(), kNoValue);
    for (std::size_t i = k; i + k < alt->size(); ++i) {
        const double dt = out.samples.t[i + k] - out.samples.t[i - k];
        if (dt > 0.0) climb[i] = ((*alt)[i + k] - (*alt)[i - k]) / dt;
    }
    out.samples.set("climbMps", std::move(climb));
}

Timeline TimelineBuilder::build(const FlightLog& log) const {
    Timeline out;
    out.samples.t = instants(log);
    out.hz = out.samples.t.size() > 1 ? 1.0 / (out.samples.t[1] - out.samples.t[0]) : options_.hz;
    addSeries(log, out);
    addEnergy(log, out);
    addGps(log, out);
    addArmed(log, out);
    addAltitude(log, out);
    addClimb(out);
    return out;
}

}  // namespace rfflightlog
