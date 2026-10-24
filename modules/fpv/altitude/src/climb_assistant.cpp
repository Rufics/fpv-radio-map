#include "climb_assistant.hpp"

#include "climb_planner.hpp"
#include "energy_model.hpp"
#include "profile_fit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

namespace fpv::altitude {
namespace {

constexpr const char* kId = "fpv.altitude";
constexpr double kInf = std::numeric_limits<double>::infinity();
/// A plan climbing faster than this over the drone's rate is warned about.
constexpr double kRateSlackMps = 0.05;
/// Below these the plan is as good as the energy-optimal profile: no advice.
constexpr double kMinSavingM = 10.0;
constexpr double kMinSavingShare = 0.1;
constexpr double kCorridorToleranceM = 0.05;

std::string km(double m) { return "km " + fixed(m / 1000.0, 2); }

std::string minutes(double s) {
    const auto whole = static_cast<long>(std::lround(s));
    return std::to_string(whole / 60) + ":" + (whole % 60 < 10 ? "0" : "") + std::to_string(whole % 60);
}

std::string lowerFirst(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

std::string signedM(double m) { return (m >= 0.0 ? "+" : "") + fixed(m, 0) + " m"; }

/// The drone as the assistant uses it: rates, speed, and the energy model when the drone is known.
struct Performance {
    std::optional<battery::EnergyModel> energy;
    double climbMps = ClimbSettings::kDefaultClimbMps;
    double descentMps = 4.0;
    double airspeedMps = ClimbSettings::kDefaultSpeedMps;
    double cruiseWh = 0.0;  ///< level flight along the whole route; the same for every profile
    json notes = json::array();

    [[nodiscard]] double groundSpeed(double courseDeg) const {
        if (!energy) return airspeedMps;
        try {
            return energy->groundSpeedMps(courseDeg);
        } catch (const battery::Unflyable&) {
            return airspeedMps;
        }
    }
    [[nodiscard]] std::optional<double> wh(double riseM) const {
        if (!energy) return std::nullopt;
        return cruiseWh + energy->verticalClimbWh(riseM);
    }
};

Performance performance(const json& request, const Route& route, const IWorld& world, const ClimbSettings& set,
                        const std::optional<json>& drone) {
    Performance p;
    double droneClimb = 0.0;
    if (drone.has_value()) {
        try {
            const battery::DroneSpec spec = battery::DroneSpec::fromJson(*drone);
            if (spec.climbMethod == "measured") droneClimb = spec.climbRateMps;
            p.airspeedMps = spec.cruiseAirspeedMps;
            p.energy.emplace(spec, battery::Air::fromJson(request.value("weather", json()), json::object()), world);
            for (const Leg& leg : route.legs()) p.cruiseWh += p.energy->levelWh(leg.from.position, leg.to.position);
            p.notes.push_back("Energy: cruise at " + fixed(p.airspeedMps, 1) + " m/s airspeed in the forecast wind, plus climbing; descents give nothing back.");
        } catch (const std::exception& e) {
            p.energy.reset();
            p.notes.push_back(std::string("Energy not estimated: ") + e.what() + ".");
        }
    } else {
        p.notes.push_back("No drone picked: energy is not estimated, and the profiles assume " + fixed(p.airspeedMps, 0) + " m/s.");
    }
    if (set.speedMps > 0.0) p.airspeedMps = set.speedMps;
    if (set.climbRateMps > 0.0) {
        p.climbMps = set.climbRateMps;
    } else if (droneClimb > 0.0) {
        p.climbMps = droneClimb;
    } else {
        p.notes.push_back("Climb rate " + fixed(p.climbMps, 1) + " m/s assumed: set climbRateMps, or measure the drone's climb in the library.");
    }
    p.descentMps = set.descentRateMps;
    return p;
}

/// The drone's limits between neighbouring samples; free in the take-off zone, where it climbs
/// and lands vertically.
std::vector<Segment> segments(const std::vector<CorridorPoint>& pts, const Performance& perf) {
    std::vector<Segment> out;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const double ds = pts[i + 1].s - pts[i].s;
        double gs = perf.groundSpeed(pts[i + 1].p.courseDeg);
        if (!(gs > 0.0)) gs = perf.airspeedMps;
        const bool free = pts[i].nearPilot || pts[i + 1].nearPilot;
        out.push_back({free ? kInf : perf.climbMps * ds / gs, free ? kInf : perf.descentMps * ds / gs, gs});
    }
    return out;
}

json statsJson(const ProfileStats& st, const Performance& perf, std::size_t waypoints) {
    json j = {{"riseM", st.riseM},
              {"timeS", st.timeS},
              {"minClearanceM", st.minClearanceM},
              {"minClearanceAtM", st.minClearanceAtM},
              {"overCeilingM", st.overCeilingM},
              {"steepestClimbMps", st.steepestClimbMps},
              {"steepestClimbAtM", st.steepestClimbAtM},
              {"waypoints", waypoints}};
    if (const auto wh = perf.wh(st.riseM)) j["energyWh"] = *wh;
    return j;
}

/// What a strategy's fix changes, now and after.
json previewRows(const ProfileStats& now, const ProfileStats& after, const Performance& perf, std::size_t nowWps, std::size_t afterWps) {
    json rows = {{{"label", "Total climb"}, {"now", fixed(now.riseM, 0) + " m"}, {"after", fixed(after.riseM, 0) + " m"}}};
    if (const auto a = perf.wh(now.riseM), b = perf.wh(after.riseM); a && b) {
        rows.push_back({{"label", "Route energy"}, {"now", fixed(*a, 1) + " Wh"}, {"after", fixed(*b, 1) + " Wh"}});
    }
    rows.push_back({{"label", "Flight time"}, {"now", minutes(now.timeS)}, {"after", minutes(after.timeS)}});
    rows.push_back({{"label", "Lowest height over the safe floor"}, {"now", signedM(now.minClearanceM)}, {"after", signedM(after.minClearanceM)}});
    rows.push_back({{"label", "Steepest climb"}, {"now", fixed(now.steepestClimbMps, 1) + " m/s"}, {"after", fixed(after.steepestClimbMps, 1) + " m/s"}});
    rows.push_back({{"label", "Waypoints"}, {"now", std::to_string(nowWps)}, {"after", std::to_string(afterWps)}});
    return rows;
}

struct Strategy {
    ClimbProfile profile;
    std::string title;
    std::string summary;
};

/// The highest height of a profile above the launch ground.
double topAboveLaunch(const std::vector<double>& mslM, const Route& route) {
    return *std::ranges::max_element(mslM) - route.altitude().launchGroundMslM();
}

std::vector<Strategy> strategies(const ClimbPlanner& planner, const Route& route, const Performance& perf, bool returns) {
    std::vector<Strategy> out;
    ClimbProfile energy = planner.energyOptimal();
    const std::string rate = fixed(perf.climbMps, 1) + " m/s";
    std::string energyText = energy.withinClimbRate
                                 ? "The least total climb through the corridor, climbing at most " + rate + " past the take-off zone."
                                 : "No profile through the corridor keeps to the drone's " + rate + " climb; this one has the least total climb.";
    out.push_back({std::move(energy), "Energy-optimal climb", std::move(energyText)});
    ClimbProfile over = planner.overLaunch();
    const std::string overTop = fixed(topAboveLaunch(over.mslM, route), 0) + " m";
    out.push_back({std::move(over), "Climb over launch", "Climb to " + overTop + " above launch over the pilot, then fly level."});
    ClimbProfile gradual = planner.gradual();
    const std::string top = fixed(topAboveLaunch(gradual.mslM, route), 0) + " m";
    out.push_back({std::move(gradual), "Gradual up and down",
                   returns ? "Climb steadily to " + top + " above launch and descend steadily back to the pilot."
                           : "Climb steadily to " + top + " above launch, then fly level."});
    return out;
}

/// Legs where the plan climbs faster than the drone can, outside the take-off zone.
void steepLegReasons(const Route& route, const std::vector<CorridorPoint>& pts, const std::vector<Segment>& seg,
                     const Performance& perf, const json& fix, json& reasons) {
    struct Worst {
        double rate;
        double atM;
        double groundSpeed;
    };
    std::map<std::size_t, Worst> worst;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const double ds = pts[i + 1].s - pts[i].s;
        if (!std::isfinite(seg[i].maxRiseM) || !(ds > 0.0)) continue;
        const double rate = (pts[i + 1].plannedMslM - pts[i].plannedMslM) / ds * seg[i].groundSpeedMps;
        const std::size_t leg = pts[i + 1].p.leg;
        if (rate > perf.climbMps + kRateSlackMps && (!worst.contains(leg) || rate > worst[leg].rate)) worst[leg] = {rate, pts[i].s, seg[i].groundSpeedMps};
    }
    for (const auto& [leg, w] : worst) {
        const Leg& l = route.legs()[leg];
        json r = reason(kId, "warn",
                        "Leg " + l.from.label + " → " + l.to.label + ": the plan climbs at " + fixed(w.rate, 1) + " m/s at " + km(w.atM) +
                            " (" + fixed(w.groundSpeed, 1) + " m/s over the ground); the drone climbs " + fixed(perf.climbMps, 1) + " m/s.",
                        fix);
        r["leg"] = leg;
        reasons.push_back(std::move(r));
    }
}

}  // namespace

ClimbSettings ClimbSettings::fromOptions(const json& options) {
    const auto bounded = [&options](const char* key, double fallback, double lo, double hi) {
        const double v = options.value(key, fallback);
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    ClimbSettings s;
    s.enabled = options.value("climbAssist", s.enabled);
    s.climbRateMps = bounded("climbRateMps", s.climbRateMps, 0.0, 50.0);
    s.descentRateMps = bounded("descentRateMps", s.descentRateMps, 0.1, 50.0);
    s.speedMps = bounded("speedMps", s.speedMps, 0.0, 100.0);
    s.heightStepM = bounded("heightStepM", s.heightStepM, 1.0, 100.0);
    s.fitToleranceM = bounded("fitToleranceM", s.fitToleranceM, 0.5, 100.0);
    return s;
}

ClimbAdvice climbAssist(const json& request, const Route& route, const IWorld& world, const std::vector<CorridorPoint>& pts,
                        const ClimbSettings& set, const std::optional<json>& drone) {
    ClimbAdvice out;
    const Performance perf = performance(request, route, world, set, drone);
    const std::vector<Segment> seg = segments(pts, perf);
    const bool returns = route.legs().back().isReturn;
    const ClimbPlanner planner(pts, seg, returns, set.heightStepM);
    const json& waypoints = request.at("mission").at("waypoints");
    const ProfileFitter fitter(pts, route, waypoints, set.fitToleranceM);

    std::vector<double> planned;
    for (const CorridorPoint& p : pts) planned.push_back(p.plannedMslM);
    const ProfileStats now = planner.stats(planned);
    const std::size_t nowWps = waypoints.size();

    json list = json::array();
    json energyFix = nullptr;
    ProfileStats energyStats;
    for (const Strategy& st : strategies(planner, route, perf, returns)) {
        const FittedRoute fit = fitter.fit(st.profile.mslM);
        const ProfileStats after = planner.stats(fit.mslM);
        const std::size_t afterWps = fit.waypoints.size();
        json fix = {{"id", std::string(kId) + "/climb-" + st.profile.id},
                    {"title", "Fly the " + lowerFirst(st.title)},
                    {"patch", {{"waypoints", fit.waypoints}}},
                    {"preview", previewRows(now, after, perf, nowWps, afterWps)}};
        json mslM = json::array();
        for (const double v : fit.mslM) mslM.push_back(std::round(v * 10.0) / 10.0);
        json item = statsJson(after, perf, afterWps);
        item["id"] = st.profile.id;
        item["title"] = st.title;
        item["summary"] = st.summary;
        item["withinClimbRate"] = st.profile.withinClimbRate && after.steepestClimbMps <= perf.climbMps + kRateSlackMps;
        item["addedWaypoints"] = fit.added;
        item["mslM"] = std::move(mslM);
        item["fix"] = fix;
        if (st.profile.id == "energy") {
            energyFix = fix;
            energyStats = after;
        }
        list.push_back(std::move(item));
    }

    steepLegReasons(route, pts, seg, perf, energyFix, out.reasons);
    const bool outside = now.minClearanceM < -kCorridorToleranceM || now.overCeilingM > kCorridorToleranceM;
    const bool inside = energyStats.minClearanceM >= -kCorridorToleranceM && energyStats.overCeilingM <= kCorridorToleranceM;
    const double saving = now.riseM - energyStats.riseM;
    if ((outside && inside) || saving >= std::max(kMinSavingM, kMinSavingShare * now.riseM)) {
        std::string text = "Climb assistant: the energy-optimal profile climbs " + fixed(energyStats.riseM, 0) + " m in all instead of " +
                           fixed(now.riseM, 0) + " m";
        if (const auto a = perf.wh(now.riseM), b = perf.wh(energyStats.riseM); a && b) text += " (" + fixed(*a - *b, 1) + " Wh less)";
        text += outside && inside ? " and keeps the whole route inside the corridor." : ".";
        out.reasons.push_back(reason(kId, "info", text, energyFix));
    }

    out.data = {{"recommended", "energy"},
                {"climbRateMps", perf.climbMps},
                {"descentRateMps", perf.descentMps},
                {"airspeedMps", perf.airspeedMps},
                {"energyEstimated", perf.energy.has_value()},
                {"heightStepM", set.heightStepM},
                {"fitToleranceM", set.fitToleranceM},
                {"plan", statsJson(now, perf, nowWps)},
                {"strategies", std::move(list)},
                {"notes", perf.notes}};
    if (drone.has_value()) out.data["drone"] = {{"id", drone->value("id", "")}, {"version", drone->value("version", 0)}};
    out.data["notes"].push_back("Past the take-off zone the profiles climb at most " + fixed(perf.climbMps, 1) + " m/s and descend at most " +
                                fixed(perf.descentMps, 1) + " m/s at the ground speed on each stretch; inside it the drone climbs and lands vertically.");
    return out;
}

}  // namespace fpv::altitude
