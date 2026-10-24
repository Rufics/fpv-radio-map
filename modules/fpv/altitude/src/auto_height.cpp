#include "auto_height.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fpv::altitude {
namespace {

constexpr const char* kId = "fpv.altitude";
/// A sample this close to a fixed end of its leg cannot be helped by the other end.
constexpr double kMinWeight = 0.05;
constexpr double kEpsilonM = 1e-6;
constexpr double kInf = std::numeric_limits<double>::infinity();

std::string km(double m) { return "km " + fixed(m / 1000.0, 2); }

/// "the control link's Fresnel clearance", "terrain clearance", "the ceiling".
std::string limitText(const std::string& by) {
    if (by == "terrain") return "terrain clearance";
    if (by == "ceiling") return "the ceiling";
    return "the " + by + " link's Fresnel clearance";
}

}  // namespace

AutoHeightPlanner::AutoHeightPlanner(const Mission& mission, const IWorld& world, const Settings& set,
                                     const LinkInputs& links, double k)
    : mission_(mission), world_(world), set_(set), links_(links), k_(k) {}

std::vector<AutoHeightPlanner::Sample> AutoHeightPlanner::samples() const {
    const Route route(mission_, world_);
    const CorridorBuilder builder(mission_, route, world_, set_, links_, k_);
    std::vector<Sample> out;
    for (const CorridorPoint& c : builder.build()) {
        const Leg& leg = route.legs()[c.p.leg];
        const double t = leg.lengthM > 0.0 ? std::clamp((c.s - leg.startM) / leg.lengthM, 0.0, 1.0) : 1.0;
        out.push_back({c.p.leg, t, c.s, route.altitude().heightFor(c.floorMslM(), c.groundMslM),
                       route.altitude().heightFor(c.ceilingMslM, c.groundMslM), c.floorBy(),
                       c.linkChecked && !c.linkFloorMslM.has_value()});
    }
    return out;
}

std::optional<double> AutoHeightPlanner::need(const Point& other, double toward, double floor) {
    // A free or pilot end: the leg flies level for this sample (the climb from the pilot is the
    // climb-out's, and two free ends both go up).
    if (other.free || other.pilot) return floor;
    // A manual end: the line from it has to pass over the floor here.
    if (toward < kMinWeight) return std::nullopt;  // right by the fixed end: not ours to fix
    return other.height + (floor - other.height) / toward;
}

std::vector<AutoHeight> AutoHeightPlanner::plan() const {
    const std::vector<Sample> xs = samples();
    // Route points: the pilot, the waypoints, and the pilot again after a return leg.
    const std::size_t nw = mission_.waypoints.size();
    std::vector<Point> pts;
    pts.push_back({mission_.pilot.heightM, false, true});
    for (const Waypoint& w : mission_.waypoints) pts.push_back({w.autoHeight ? -kInf : w.heightM, w.autoHeight, false});
    if (mission_.returnToPilot) pts.push_back({mission_.pilot.heightM, false, true});
    // Each free point takes the highest of what the samples on its two legs need of it.
    std::vector<Point> lifted = pts;
    const auto raise = [&lifted](std::size_t i, std::optional<double> h, const Sample& x) {
        if (!h.has_value() || *h <= lifted[i].height) return;
        lifted[i].height = *h;
        lifted[i].by = x.by;
        lifted[i].atM = x.s;
    };
    for (const Sample& x : xs) {
        if (pts[x.leg + 1].free) raise(x.leg + 1, need(pts[x.leg], x.t, x.floor), x);
        if (pts[x.leg].free) raise(x.leg, need(pts[x.leg + 1], 1.0 - x.t, x.floor), x);
    }

    std::vector<AutoHeight> out;
    for (std::size_t i = 0; i < nw; ++i) {
        const Point& p = lifted[i + 1];
        if (!p.free || !std::isfinite(p.height)) continue;
        AutoHeight h{i, p.height, p.by, p.atM, false};
        for (const Sample& x : xs) {
            const bool own = x.leg == i && x.t >= 1.0;
            if (own && h.heightM > x.ceiling + kEpsilonM) {
                h = {i, x.ceiling, "ceiling", x.s, true};
            }
            const bool adjacent = x.leg == i || x.leg == i + 1;
            if (adjacent && x.noLink && !h.blocked) {
                h.blocked = true;
                h.atM = x.s;
            }
        }
        h.heightM = std::ceil(h.heightM - kEpsilonM);
        out.push_back(h);
    }
    return out;
}

json prepare(const json& request, const IWorld& world, const LinkInputs& links) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    json waypoints = json::array();
    json reasons = json::array();
    const bool wanted = std::ranges::any_of(mission.waypoints, &Waypoint::autoHeight);
    if (!options.value("autoHeight", true) || !wanted) return {{"waypoints", waypoints}, {"reasons", reasons}};
    const Settings set = Settings::fromOptions(options);
    const double k = refraction(request.value("weather", json())).first;
    const std::string field(AltitudeModel::waypointField(mission.altitudeRef));
    const std::string unit = mission.altitudeRef == AltitudeReference::Launch ? " m above launch" : " m AGL";
    try {
        for (const AutoHeight& h : AutoHeightPlanner(mission, world, set, links, k).plan()) {
            const std::string name = mission.waypoints[h.waypoint].label;
            waypoints.push_back({{"index", h.waypoint}, {"field", field}, {"heightM", h.heightM},
                                 {"limitedBy", h.limitedBy}, {"atM", h.atM}, {"blocked", h.blocked}});
            if (!h.blocked) continue;
            const std::string why =
                h.limitedBy == "ceiling"
                    ? "its legs need more than the " + fixed(set.ceilingAglM, 0) + " m AGL ceiling allows; set to the ceiling"
                    : "no height up to " + fixed(set.maxAglM, 0) + " m AGL keeps " + fixed(set.fraction * 100.0, 0) +
                          " % of the Fresnel zone clear at " + km(h.atM) + "; the height keeps " + limitText(h.limitedBy) + " only";
            json r = reason(kId, "warn", name + ": automatic height " + fixed(h.heightM, 0) + unit + ", but " + why +
                                             ". A relay (fpv.relay) or another launch point (fpv.best-launch) may help.");
            r["leg"] = h.waypoint;
            reasons.push_back(std::move(r));
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Automatic heights not set: ") + e.what() + "."));
    }
    return {{"waypoints", waypoints}, {"reasons", reasons}};
}

}  // namespace fpv::altitude
