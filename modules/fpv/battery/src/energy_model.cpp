#include "energy_model.hpp"

#include <rfcore/constants.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace fpv::battery {
namespace {

constexpr double kDeg = rfcore::constants::kPi / 180.0;

// Adaptive Simpson to a relative tolerance close to double precision. The integrand is smooth
// (the course along a geodesic changes slowly), so this converges to the exact integral.
double simpson(const std::function<double(double)>& f, double a, double b, double fa, double fm, double fb,
               double whole, double tol, int depth) {
    const double m = (a + b) / 2.0;
    const double lm = (a + m) / 2.0, rm = (m + b) / 2.0;
    const double flm = f(lm), frm = f(rm);
    const double left = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
    const double right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
    const double delta = left + right - whole;
    if (depth <= 0 || std::abs(delta) <= 15.0 * tol) return left + right + delta / 15.0;
    return simpson(f, a, m, fa, flm, fm, left, tol / 2.0, depth - 1) +
           simpson(f, m, b, fm, frm, fb, right, tol / 2.0, depth - 1);
}

double integrate(const std::function<double(double)>& f, double a, double b) {
    if (b <= a) return 0.0;
    const double fa = f(a), fb = f(b), fm = f((a + b) / 2.0);
    const double whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
    return simpson(f, a, b, fa, fm, fb, whole, std::abs(whole) * 1e-12 + 1e-15, 40);
}

}  // namespace

DroneSpec DroneSpec::fromJson(const json& j) {
    DroneSpec d;
    d.id = j.value("id", "");
    d.version = j.value("version", 0);
    d.massKg = j.at("massKg").get<double>();
    d.cruiseAirspeedMps = j.at("cruiseAirspeedMps").get<double>();
    d.cruisePowerW = j.at("cruisePowerW").get<double>();
    d.powerDensityExponent = j.value("powerDensityExponent", 0.0);
    const json climb = j.value("climb", json{{"method", "potential-energy"}});
    d.climbMethod = climb.at("method").get<std::string>();
    d.climbEfficiency = climb.value("efficiency", 0.0);
    d.climbPowerW = climb.value("climbPowerW", 0.0);
    d.climbRateMps = climb.value("climbRateMps", 0.0);
    if (d.climbMethod == "potential-energy" && !(d.climbEfficiency > 0.0)) {
        throw rfmod::BadRequest("drone " + d.id + ": climb efficiency is required for the potential-energy method");
    }
    if (d.climbMethod == "measured" && (!(d.climbRateMps > 0.0) || d.climbPowerW < d.cruisePowerW)) {
        throw rfmod::BadRequest("drone " + d.id + ": measured climb needs climbRateMps > 0 and climbPowerW >= cruisePowerW");
    }
    return d;
}

BatterySpec BatterySpec::fromJson(const json& j) {
    BatterySpec b;
    b.id = j.value("id", "");
    b.version = j.value("version", 0);
    b.cellsSeries = j.at("cellsSeries").get<int>();
    b.capacityAh = j.at("capacityAh").get<double>();
    b.nominalCellVoltage = j.at("nominalCellVoltage").get<double>();
    b.usableFraction = j.at("usableFraction").get<double>();
    for (const json& p : j.value("temperatureDerating", json::array())) {
        b.derating.emplace_back(p.at("celsius").get<double>(), p.at("factor").get<double>());
    }
    std::sort(b.derating.begin(), b.derating.end());
    return b;
}

BatterySpec::Derated BatterySpec::deratingAt(std::optional<double> t) const {
    if (derating.empty()) return {1.0, "no temperature data for this battery: capacity not derated"};
    if (!t.has_value()) return {1.0, "air temperature unknown: capacity not derated"};
    if (*t < derating.front().first || *t > derating.back().first) {
        throw std::domain_error("air temperature " + fixed(*t, 1) + " °C is outside the battery's measured range (" +
                                fixed(derating.front().first, 1) + " to " + fixed(derating.back().first, 1) +
                                " °C); not extrapolated");
    }
    for (std::size_t i = 0; i + 1 < derating.size(); ++i) {
        const auto [t0, f0] = derating[i];
        const auto [t1, f1] = derating[i + 1];
        if (*t >= t0 && *t <= t1) {
            if (*t == t0) return {f0, "measured factor at " + fixed(t0, 1) + " °C"};
            if (*t == t1) return {f1, "measured factor at " + fixed(t1, 1) + " °C"};
            const double f = f0 + (f1 - f0) * (*t - t0) / (t1 - t0);
            return {f, "interpolated linearly between measured points at " + fixed(t0, 1) + " and " + fixed(t1, 1) + " °C"};
        }
    }
    return {derating.back().second, "measured factor"};  // single point equal to t
}

EnergyModel::EnergyModel(DroneSpec drone, Air air, const IWorld& world)
    : drone_(std::move(drone)), air_(air), world_(world), power_(0.0) {
    power_ = cruisePowerW();
}

double EnergyModel::cruisePowerW() const {
    if (drone_.powerDensityExponent == 0.0) return drone_.cruisePowerW;
    if (!air_.densityKgM3.has_value()) throw std::domain_error("air density unknown, but this drone's power depends on it");
    return drone_.cruisePowerW * std::pow(kSeaLevelDensity / *air_.densityKgM3, drone_.powerDensityExponent);
}

double EnergyModel::groundSpeedMps(double courseDeg) const {
    const double va = drone_.cruiseAirspeedMps;
    if (!air_.windSpeedMps.has_value() || !air_.windFromDeg.has_value() || *air_.windSpeedMps == 0.0) return va;
    // Wind blows towards from+180°. Split it along and across the course.
    const double rel = (*air_.windFromDeg + 180.0 - courseDeg) * kDeg;
    const double along = *air_.windSpeedMps * std::cos(rel);
    const double across = *air_.windSpeedMps * std::sin(rel);
    if (std::abs(across) >= va) {
        throw Unflyable("crosswind " + fixed(std::abs(across), 1) + " m/s is at or above the airspeed " + fixed(va, 1) + " m/s");
    }
    const double vg = along + std::sqrt(va * va - across * across);
    if (!(vg > 0.0)) throw Unflyable("headwind " + fixed(-along, 1) + " m/s stops the drone at " + fixed(va, 1) + " m/s airspeed");
    return vg;
}

double EnergyModel::cruiseWh(const LatLon& a, double azimuthDeg, double length) const {
    // ∫ P / Vg(course(x)) dx along the geodesic; the course at x comes from the geodesic itself.
    const auto seconds = integrate(
        [&](double x) { return 1.0 / groundSpeedMps(world_.direct(a, azimuthDeg, x).azimuthDeg); }, 0.0, length);
    return power_ * seconds / 3600.0;
}

double EnergyModel::climbWh(const LatLon& a, double aglA, const LatLon& b, double aglB) const {
    const std::vector<GroundSample> g = world_.groundProfile(a, b);
    if (g.size() < 2) return 0.0;
    const double total = g.back().distanceM;
    double rise = 0.0;
    double prev = g.front().groundMslM + aglA;
    for (std::size_t i = 1; i < g.size(); ++i) {
        const double t = total > 0.0 ? g[i].distanceM / total : 1.0;
        const double msl = g[i].groundMslM + aglA + t * (aglB - aglA);
        rise += std::max(0.0, msl - prev);  // descending gives nothing back
        prev = msl;
    }
    if (drone_.climbMethod == "measured") {
        return (drone_.climbPowerW - power_) * (rise / drone_.climbRateMps) / 3600.0;
    }
    return drone_.massKg * kGravity * rise / drone_.climbEfficiency / 3600.0;
}

double EnergyModel::segmentWh(const LatLon& a, double aglA, const LatLon& b, double aglB) const {
    const InverseResult inv = world_.inverse(a, b);
    return cruiseWh(a, inv.azimuth1Deg, inv.distanceM) + climbWh(a, aglA, b, aglB);
}

double EnergyModel::levelWh(const LatLon& a, const LatLon& b) const {
    const InverseResult inv = world_.inverse(a, b);
    return cruiseWh(a, inv.azimuth1Deg, inv.distanceM);
}

double EnergyModel::verticalClimbWh(double riseM) const {
    if (riseM <= 0.0) return 0.0;
    if (drone_.climbMethod == "measured") return drone_.climbPowerW * (riseM / drone_.climbRateMps) / 3600.0;
    return drone_.massKg * kGravity * riseM / drone_.climbEfficiency / 3600.0;
}

double EnergyModel::routeWh(const Route& route, double s) const {
    double e = 0.0;
    for (const Leg& leg : route.legs()) {
        if (s <= leg.startM) break;
        if (s >= leg.startM + leg.lengthM) {
            e += cruiseWh(leg.from.position, leg.azimuthDeg, leg.lengthM) +
                 climbWh(leg.from.position, leg.from.aglM, leg.to.position, leg.to.aglM);
        } else {
            const RoutePoint p = route.at(s);
            e += cruiseWh(leg.from.position, leg.azimuthDeg, s - leg.startM) +
                 climbWh(leg.from.position, leg.from.aglM, p.position, p.aglM);
        }
    }
    return e;
}

Plan BatteryPlanner::plan(const Mission& mission, const Route& route, const IWorld& world,
                          std::optional<double> temperatureC) const {
    Plan p{};
    const auto der = battery_.deratingAt(temperatureC);
    p.energyWh = battery_.nominalWh() * der.factor;
    p.usableWh = p.energyWh * battery_.usableFraction;
    p.notes.push_back("Battery capacity: " + der.note);
    p.neededM = route.length();
    p.neededWh = model_.routeWh(route, route.length());

    // Range: energy along the route only grows, so bisection finds where it runs out exactly.
    if (p.neededWh <= p.usableWh) {
        p.rangeM = route.length();
    } else {
        double lo = 0.0, hi = route.length();
        while (hi - lo > 1e-3) {
            const double mid = (lo + hi) / 2.0;
            (model_.routeWh(route, mid) <= p.usableWh ? lo : hi) = mid;
        }
        p.rangeM = lo;
    }
    p.shortfallM = std::max(0.0, p.neededM - p.rangeM);

    if (!mission.returnToPilot || p.neededWh <= p.usableWh) return p;

    // Turn-back: the farthest point on the way out from which the direct flight home still fits.
    auto costAt = [&](double s) {
        const RoutePoint pt = route.at(s);
        return model_.routeWh(route, s) + model_.segmentWh(pt.position, pt.aglM, mission.pilot.position, mission.pilot.aglM);
    };
    const double out = route.outboundLength();
    double lastOk = -1.0, firstBad = -1.0;
    for (std::size_t i = 0;; ++i) {
        const double s = std::min(out, static_cast<double>(i) * step_);
        if (costAt(s) <= p.usableWh) {
            lastOk = s;
            firstBad = -1.0;
        } else if (lastOk >= 0.0 && firstBad < 0.0) {
            firstBad = s;
        }
        if (s >= out) break;
    }
    if (lastOk < 0.0) {
        p.notes.emplace_back("Even turning back at the start does not fit in the usable energy.");
        return p;
    }
    if (firstBad > lastOk) {
        double lo = lastOk, hi = firstBad;
        while (hi - lo > 1e-3) {
            const double mid = (lo + hi) / 2.0;
            (costAt(mid) <= p.usableWh ? lo : hi) = mid;
        }
        lastOk = lo;
    }
    const RoutePoint pt = route.at(lastOk);
    p.turnBack = TurnBack{lastOk, world.inverse(pt.position, mission.pilot.position).distanceM, pt.position, pt.aglM, pt.leg};
    p.notes.push_back("Turn-back point searched every " + fixed(step_, 0) + " m along the route, then refined to 1 mm.");
    return p;
}

}  // namespace fpv::battery
