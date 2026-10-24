#include "envelope.hpp"

#include <algorithm>
#include <cmath>

namespace fpv::airframe {
namespace {

constexpr const char* kId = "fpv.airframe";
/// Cruise below this multiple of the turning stall speed leaves little margin for gusts.
constexpr double kStallMargin = 1.2;

std::optional<double> number(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_number()) return j.at(key).get<double>();
    return std::nullopt;
}

/// The heading change from `fromDeg` to `toDeg`, 0 to 180°.
double headingChange(double fromDeg, double toDeg) {
    const double d = std::fmod(std::fmod(toDeg - fromDeg, 360.0) + 540.0, 360.0) - 180.0;
    return std::abs(d);
}

std::string km(double m) { return fixed(m / 1000.0, 2) + " km"; }

json pointJson(const ProfilePoint& p) {
    return {{"distanceAlongRouteM", p.alongM}, {"lat", p.position.lat}, {"lon", p.position.lon}, {"leg", p.leg}};
}

/// The reasons and data of one analysis, filled check by check.
class EnvelopeReport {
public:
    EnvelopeReport(const json& drone, const Airframe& airframe)
        : airframe_(airframe), airspeed_(drone.at("cruiseAirspeedMps").get<double>()) {
        data_["airframe"] = std::string(airframe.name());
        data_["drone"] = {{"id", drone.value("id", "")}, {"version", drone.value("version", 0)}};
    }

    [[nodiscard]] json result() const { return {{"reasons", reasons_}, {"data", data_}}; }

    void add(const std::string& severity, const std::string& text, const json& leg = nullptr) {
        json r = reason(kId, severity, text);
        if (!leg.is_null()) r["leg"] = leg;
        reasons_.push_back(std::move(r));
    }

    void multirotor() {
        add("info", "Multirotor: it hovers, climbs in place and turns on the spot, so there are no fixed-wing limits to check.");
    }

    [[nodiscard]] double airspeedMps() const { return airspeed_; }

    /// The cruise airspeed against the stall in a turn at the envelope's bank, and the wind.
    void speeds(std::optional<double> windMps) {
        const double stall = airframe_.turningStallSpeedMps();
        const FixedWingEnvelope& e = airframe_.envelope();
        data_["speeds"] = {{"cruiseAirspeedMps", airspeed_}, {"stallSpeedMps", e.stallSpeedMps}, {"turningStallSpeedMps", stall}};
        const std::string bank = fixed(e.maxBankDeg, 0) + "° bank";
        if (airspeed_ <= stall) {
            add("blocked", "Fixed-wing: at " + bank + " the plane stalls at " + fixed(stall, 1) + " m/s, at or above its cruise airspeed " +
                               fixed(airspeed_, 1) + " m/s; lower the bank angle or fly faster.");
        } else if (airspeed_ < kStallMargin * stall) {
            add("warn", "Fixed-wing: the cruise airspeed " + fixed(airspeed_, 1) + " m/s is less than " + fixed(kStallMargin, 1) +
                            " times the stall speed in a turn at " + bank + " (" + fixed(stall, 1) + " m/s); a gust in a turn can stall it.");
        }
        if (windMps.has_value()) {
            data_["speeds"]["windSpeedMps"] = *windMps;
            if (*windMps >= airspeed_) {
                add("blocked", "Fixed-wing: the wind " + fixed(*windMps, 1) + " m/s is at or above the cruise airspeed " + fixed(airspeed_, 1) +
                                   " m/s; the plane cannot make headway into it and cannot hover to wait.");
            }
        }
    }

    void climb(const ClimbResult& r, const Route& route) {
        const FixedWingEnvelope& e = airframe_.envelope();
        json v = {{"maxClimbAngleDeg", e.maxClimbAngleDeg}, {"maxDiveAngleDeg", e.maxDiveAngleDeg}, {"arrivals", json::array()}};
        for (const LateArrival& a : r.arrivals) {
            v["arrivals"].push_back({{"leg", a.leg}, {"belowPlanM", a.belowPlanM}, {"extraClimbM", a.extraClimbM}});
            describeArrival(a, route.legs().at(a.leg));
        }
        if (r.worst.has_value()) {
            v["worst"] = pointJson(r.worst->at);
            v["worst"]["belowPlanM"] = r.worst->belowPlanM;
        }
        if (r.terrain.has_value()) {
            v["terrain"] = pointJson(r.terrain->at);
            v["terrain"]["gapM"] = r.terrainGapM;
            describeTerrain(*r.terrain, r.terrainGapM);
        }
        if (!r.worst.has_value()) {
            add("info", "Fixed-wing: the plane can follow the planned heights at its " + fixed(e.maxClimbAngleDeg, 0) + "° climb and " +
                            fixed(e.maxDiveAngleDeg, 0) + "° descent limits.");
        }
        data_["vertical"] = std::move(v);
    }

    void turns(const std::vector<Turn>& turns, const Route& route) {
        json list = json::array();
        std::size_t tight = 0;
        for (const Turn& t : turns) {
            const Leg& in = route.legs().at(t.legIn);
            list.push_back({{"leg", t.legIn}, {"waypoint", in.to.label}, {"headingChangeDeg", t.headingChangeDeg}, {"radiusM", t.radiusM},
                            {"leadM", t.leadM}, {"insideM", t.insideM}, {"uTurn", t.uTurn}, {"fits", t.fits}});
            if (!t.fits) {
                ++tight;
                describeTightTurn(t, in, route.legs().at(t.legIn + 1));
            } else if (t.uTurn) {
                add("info", "Turn at " + in.to.label + ": a " + fixed(t.headingChangeDeg, 0) + "° turn-around; the plane flies past it and swings up to " +
                                fixed(2.0 * t.radiusM, 0) + " m off the route.",
                    t.legIn);
            }
        }
        data_["turns"] = std::move(list);
        if (!turns.empty() && tight == 0) {
            add("info", "Fixed-wing: every turn fits its legs at a " + fixed(turns.front().radiusM, 0) + " m turn radius.");
        }
    }

private:
    void describeArrival(const LateArrival& a, const Leg& leg) {
        const double lap = 2.0 * rfcore::constants::kPi * airframe_.loiterRadiusM(airspeed_) * airframe_.climbGradient();
        add("warn",
            "Fixed-wing: the plane reaches " + leg.to.label + " " + fixed(a.belowPlanM, 0) + " m below the planned height; at its " +
                fixed(airframe_.envelope().maxClimbAngleDeg, 0) + "° climb it needs another " + fixed(a.extraClimbM, 0) +
                " m of flight. Lengthen the leg, add a climb circle (each lap gains about " + fixed(lap, 0) + " m) or plan it lower.",
            a.leg);
    }

    void describeTerrain(const Shortfall& s, double gapM) {
        const std::string text = gapM < 0.0 ? "flies into terrain or clutter" : "passes " + fixed(gapM, 0) + " m over terrain or clutter";
        add(gapM < 0.0 ? "blocked" : "warn",
            "Fixed-wing: climbing at most " + fixed(airframe_.envelope().maxClimbAngleDeg, 0) + "°, the plane is " + fixed(s.belowPlanM, 0) +
                " m below the plan at " + km(s.at.alongM) + " along the route and " + text + ".",
            s.at.leg);
    }

    void describeTightTurn(const Turn& t, const Leg& in, const Leg& out) {
        const double shorter = std::min(in.lengthM, out.lengthM);
        add("warn",
            "Turn at " + in.to.label + ": " + fixed(t.headingChangeDeg, 0) + "° at a " + fixed(t.radiusM, 0) + " m radius starts " +
                fixed(t.leadM, 0) + " m before the waypoint, more than half of the " + fixed(shorter, 0) +
                " m leg; the plane overshoots and joins the next leg off the route.",
            t.legIn);
    }

    const Airframe& airframe_;
    double airspeed_;
    json reasons_ = json::array();
    json data_ = json::object();
};

}  // namespace

std::vector<ProfilePoint> VerticalProfile::of(const Route& route, const IWorld& world) {
    std::vector<ProfilePoint> out;
    const AltitudeModel& altitude = route.altitude();
    for (const Leg& leg : route.legs()) {
        const std::vector<GroundSample> ground = world.groundProfile(leg.from.position, leg.to.position);
        // Every leg after the first starts where the previous one ended.
        for (std::size_t i = out.empty() ? 0 : 1; i < ground.size(); ++i) {
            const GroundSample& g = ground[i];
            const double t = leg.lengthM > 0.0 ? std::clamp(g.distanceM / leg.lengthM, 0.0, 1.0) : 1.0;
            const double height = leg.from.heightM + t * (leg.to.heightM - leg.from.heightM);
            out.push_back({leg.startM + g.distanceM, g.position, leg.index, altitude.mslM(height, g.groundMslM), g.groundMslM, g.obstacleM});
        }
    }
    return out;
}

ClimbResult ClimbSimulator::fly(const std::vector<ProfilePoint>& profile) const {
    ClimbResult out;
    if (profile.empty()) return out;
    const double up = airframe_.climbGradient();
    const double down = airframe_.diveGradient();
    double achieved = profile.front().plannedMslM;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const ProfilePoint& p = profile[i];
        if (i > 0) {
            const double dx = p.alongM - profile[i - 1].alongM;
            achieved = std::clamp(p.plannedMslM, achieved - dx * down, achieved + dx * up);
        }
        const double below = p.plannedMslM - achieved;
        if (below > tolerance_ && (!out.worst.has_value() || below > out.worst->belowPlanM)) out.worst = Shortfall{p, below};
        checkTerrain(p, achieved, out);
        if (i + 1 == profile.size() || profile[i + 1].leg != p.leg) recordArrival(p, below, out);
    }
    return out;
}

void ClimbSimulator::checkTerrain(const ProfilePoint& p, double achievedMsl, ClimbResult& out) const {
    if (out.terrain.has_value()) return;
    // Only where the climb limit puts the plane lower than planned: a plan that is itself low
    // (take-off, landing) is the pilot's choice and the server checks it against the ground.
    const double below = p.plannedMslM - achievedMsl;
    const double gap = achievedMsl - (p.groundMslM + p.obstacleM);
    if (below > tolerance_ && gap < clearance_) {
        out.terrain = Shortfall{p, below};
        out.terrainGapM = gap;
    }
}

void ClimbSimulator::recordArrival(const ProfilePoint& p, double belowPlan, ClimbResult& out) const {
    if (belowPlan <= tolerance_) return;
    out.arrivals.push_back({p.leg, belowPlan, belowPlan / airframe_.climbGradient()});
}

std::vector<Turn> TurnPlanner::turns(const Route& route, const IWorld& world) const {
    std::vector<Turn> out;
    const std::vector<Leg>& legs = route.legs();
    for (std::size_t i = 0; i + 1 < legs.size(); ++i) {
        if (!(legs[i].lengthM > 0.0) || !(legs[i + 1].lengthM > 0.0)) continue;
        const double inAzimuth = world.inverse(legs[i].from.position, legs[i].to.position).azimuth2Deg;
        out.push_back(turnAt(legs[i], legs[i + 1], inAzimuth));
    }
    return out;
}

Turn TurnPlanner::turnAt(const Leg& in, const Leg& out, double inAzimuthDeg) const {
    Turn t{};
    t.legIn = in.index;
    t.headingChangeDeg = headingChange(inAzimuthDeg, out.azimuthDeg);
    t.radiusM = airframe_.turnRadiusM(speed_);
    const double room = std::min(in.lengthM, out.lengthM) / 2.0;
    t.uTurn = t.headingChangeDeg >= kUTurnDeg;
    if (t.uTurn) {
        // Flown over the waypoint, then round: the plane needs a radius of room past it.
        t.leadM = t.radiusM;
        t.insideM = 0.0;
    } else {
        const double half = Airframe::radians(t.headingChangeDeg) / 2.0;
        t.leadM = t.radiusM * std::tan(half);
        t.insideM = t.radiusM * (1.0 / std::cos(half) - 1.0);
    }
    t.fits = t.leadM <= room;
    return t;
}

json analyze(const json& request, const IWorld& world, const json& drone) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    const Airframe airframe = Airframe::fromDrone(drone);
    EnvelopeReport report(drone, airframe);
    if (!airframe.fixedWing()) {
        report.multirotor();
        return report.result();
    }
    std::optional<double> wind = number(request.value("weather", json()), "windSpeedMps");
    if (const auto w = number(options, "windSpeedMps"); w.has_value()) wind = w;
    report.speeds(wind);
    try {
        const Route route(mission, world);
        const ClimbSimulator sim(airframe, options.value("terrainClearanceM", 10.0), options.value("toleranceM", 5.0));
        report.climb(sim.fly(VerticalProfile::of(route, world)), route);
        report.turns(TurnPlanner(airframe, report.airspeedMps() + wind.value_or(0.0)).turns(route, world), route);
    } catch (const MissingTerrain& e) {
        report.add("warn", std::string("Fixed-wing envelope not checked: ") + e.what() + ".");
    }
    return report.result();
}

}  // namespace fpv::airframe
