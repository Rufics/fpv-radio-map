#include "corridor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace fpv::altitude {
namespace {

constexpr const char* kId = "fpv.altitude";
constexpr double kScanStepM = 5.0;

double lerpGround(const GroundSample& a, const GroundSample& b, double d) {
    const double span = b.distanceM - a.distanceM;
    const double t = span > 0.0 ? std::clamp((d - a.distanceM) / span, 0.0, 1.0) : 0.0;
    return std::lerp(a.groundMslM, b.groundMslM, t);
}

std::string km(double m) { return "km " + fixed(m / 1000.0, 2); }

/// The link with the lowest frequency: its Fresnel zone is the widest, so it is the one drawn.
const std::pair<const std::string, LinkInput>* widestLink(const LinkInputs& links) {
    const std::pair<const std::string, LinkInput>* best = nullptr;
    for (const auto& entry : links) {
        if (best == nullptr || entry.second.frequency.value() < best->second.frequency.value()) best = &entry;
    }
    return best;
}

json corridorJson(const std::vector<CorridorPoint>& pts, const Settings& set) {
    json c = {{"stepM", set.stepM}, {"clearanceM", set.clearanceM}, {"takeoffM", set.takeoffM}, {"ceilingAglM", set.ceilingAglM},
              {"linkFloor", set.linkFloor}, {"fresnelFraction", set.fraction}};
    for (const char* key : {"s", "lat", "lon", "leg", "groundMslM", "plannedMslM", "floorMslM", "terrainFloorMslM",
                            "linkFloorMslM", "ceilingMslM", "fresnelRadiusM"}) {
        c[key] = json::array();
    }
    for (const CorridorPoint& p : pts) {
        c["s"].push_back(p.s);
        c["lat"].push_back(p.p.position.lat);
        c["lon"].push_back(p.p.position.lon);
        c["leg"].push_back(p.p.leg);
        c["groundMslM"].push_back(p.groundMslM);
        c["plannedMslM"].push_back(p.plannedMslM);
        c["floorMslM"].push_back(p.floorMslM());
        c["terrainFloorMslM"].push_back(p.terrainFloorMslM);
        c["linkFloorMslM"].push_back(p.linkFloorMslM.has_value() ? json(*p.linkFloorMslM) : json(nullptr));
        c["ceilingMslM"].push_back(p.ceilingMslM);
        c["fresnelRadiusM"].push_back(p.fresnelRadiusM);
    }
    return c;
}

/// Each waypoint in both references, so the view can show the other one for reference.
json waypointsJson(const Route& route, const IWorld& world) {
    json out = json::array();
    const double launch = world.groundMsl(route.legs().front().from.position);
    for (const Leg& leg : route.legs()) {
        if (leg.isReturn) continue;
        const double ground = world.groundMsl(leg.to.position);
        const double msl = route.altitude().mslM(leg.to.heightM, ground);
        out.push_back({{"index", leg.index}, {"label", leg.to.label}, {"s", leg.startM + leg.lengthM},
                       {"groundMslM", ground}, {"mslM", msl}, {"aglM", msl - ground}, {"aboveLaunchM", msl - launch}});
    }
    return out;
}

json summaryJson(const std::vector<CorridorPoint>& pts) {
    const auto ground = std::ranges::max_element(pts, {}, &CorridorPoint::groundMslM);
    const auto planned = std::ranges::max_element(pts, {}, &CorridorPoint::plannedMslM);
    const auto margin = [](const CorridorPoint& p) { return p.plannedMslM - p.floorMslM(); };
    const auto tight = std::ranges::min_element(pts, {}, margin);
    return {{"highestGroundMslM", ground->groundMslM}, {"highestGroundAtM", ground->s},
            {"highestPlannedMslM", planned->plannedMslM}, {"highestPlannedAtM", planned->s},
            {"lowestMarginM", margin(*tight)}, {"lowestMarginAtM", tight->s}};
}

/// The worst sample of one leg by `score` (larger = worse), when its score is above `limit`.
template <class Score>
std::optional<CorridorPoint> worstOnLeg(const std::vector<CorridorPoint>& pts, std::size_t leg, Score score, double limit) {
    std::optional<CorridorPoint> worst;
    for (const CorridorPoint& p : pts) {
        if (p.p.leg == leg && score(p) > limit && (!worst || score(p) > score(*worst))) worst = p;
    }
    return worst;
}

json legReasons(const Route& route, const std::vector<CorridorPoint>& pts, const Settings& set) {
    json out = json::array();
    constexpr double kTolerance = 0.05;  // display precision of the heights
    for (const Leg& leg : route.legs()) {
        const std::string name = "Leg " + leg.from.label + " → " + leg.to.label;
        const auto add = [&out, &leg](json r) {
            r["leg"] = leg.index;
            out.push_back(std::move(r));
        };
        if (const auto low = worstOnLeg(pts, leg.index, [](const CorridorPoint& p) { return p.nearPilot ? 0.0 : p.floorMslM() - p.plannedMslM; }, kTolerance)) {
            const std::string by = low->floorBy();
            const std::string limit = by == "terrain" ? fixed(set.clearanceM, 0) + " m above the ground and obstacles"
                                                      : fixed(set.fraction * 100.0, 0) + " % of the " + by + " Fresnel zone clear";
            add(reason(kId, "warn", name + ": " + fixed(low->floorMslM() - low->plannedMslM, 1) + " m under the safe corridor at " +
                                        km(low->s) + " (its floor there: " + limit + ")."));
        }
        if (const auto high = worstOnLeg(pts, leg.index, [](const CorridorPoint& p) { return p.plannedMslM - p.ceilingMslM; }, kTolerance)) {
            add(reason(kId, "warn", name + ": " + fixed(high->plannedMslM - high->groundMslM, 0) + " m above the ground at " +
                                        km(high->s) + ", over the " + fixed(set.ceilingAglM, 0) + " m ceiling."));
        }
        if (const auto blind = worstOnLeg(pts, leg.index, [](const CorridorPoint& p) { return p.linkChecked && !p.linkFloorMslM.has_value() ? 1.0 : 0.0; }, 0.5)) {
            add(reason(kId, "warn", name + ": no height up to " + fixed(set.maxAglM, 0) + " m AGL keeps " + fixed(set.fraction * 100.0, 0) +
                                        " % of the Fresnel zone clear at " + km(blind->s) + "; the floor there is the terrain clearance only."));
        }
    }
    return out;
}

}  // namespace

CorridorBuilder::CorridorBuilder(const Mission& mission, const Route& route, const IWorld& world, const Settings& set,
                                 const LinkInputs& links, double k)
    : mission_(mission), route_(route), world_(world), set_(set) {
    if (set_.linkFloor) {
        // One solve per sample and link: scan in 5 m steps (the bisection still refines to 1 mm).
        for (const auto& [kind, link] : links) solvers_.emplace_back(kind, ClearanceSolver(k, link.frequency, set_.fraction, kScanStepM));
    }
    if (const auto* w = widestLink(links)) lambda_ = rfcore::wavelength(w->second.frequency).value();
    for (const Leg& leg : route_.legs()) ground_.emplace_back(world_.groundProfile(leg.from.position, leg.to.position));
}

std::vector<CorridorPoint> CorridorBuilder::build() const {
    std::vector<CorridorPoint> out;
    for (const double s : sampleDistances(route_, set_.stepM)) out.push_back(at(s));
    return out;
}

CorridorPoint CorridorBuilder::at(double s) const {
    const RoutePoint p = route_.at(s);
    const Leg& leg = route_.legs()[p.leg];
    const LegGround& g = ground_[p.leg];
    const double along = std::clamp(s - leg.startM, 0.0, leg.lengthM);
    const double ground = g.groundAt(along);
    const InverseResult inv = world_.inverse(mission_.pilot.position, p.position);
    // The take-off zone: the clearance grows with the distance from the pilot.
    const double ramp = set_.takeoffM > 0.0 ? std::min(1.0, inv.distanceM / set_.takeoffM) : 1.0;
    CorridorPoint c{s, p, ground, route_.altitude().mslM(p.heightM, ground),
                    g.highestIn(along - set_.stepM / 2.0, along + set_.stepM / 2.0) + set_.clearanceM * ramp,
                    ramp < 1.0, std::nullopt, false, ground + set_.ceilingAglM, 0.0, {}};
    if (inv.distanceM < 1.0) return c;  // at the pilot: no path to clear
    if (lambda_ > 0.0) c.fresnelRadiusM = 0.5 * std::sqrt(lambda_ * inv.distanceM);
    if (!solvers_.empty()) {
        const LinkFloor f = linkFloor(p, inv);
        c.linkChecked = true;
        c.linkFloorMslM = f.mslM;
        c.linkFloorBy = f.by;
    }
    return c;
}

CorridorBuilder::LinkFloor CorridorBuilder::linkFloor(const RoutePoint& p, const InverseResult& inv) const {
    const DirectResult mid = world_.direct(mission_.pilot.position, inv.azimuth1Deg, inv.distanceM / 2.0);
    const std::vector<GroundSample> path = world_.groundProfile(mission_.pilot.position, p.position);
    const auto profile = ClearanceSolver::toProfile(path, mid.azimuthDeg, mid.point.lat);
    double top = -std::numeric_limits<double>::infinity();
    std::string by;
    for (const LinkSolver& l : solvers_) {
        const auto need = l.solver.requiredAgl(profile, mission_.pilot.heightM, set_.maxAglM);
        if (!need.has_value()) return {std::nullopt, l.kind};
        if (const double msl = path.back().groundMslM + *need; msl > top) {
            top = msl;
            by = l.kind;
        }
    }
    return {top, by};
}

Settings Settings::fromOptions(const json& options) {
    // The host checks options against the schema; these keep its bounds for any other caller too,
    // so a zero step can never loop forever.
    const auto bounded = [&options](const char* key, double fallback, double lo, double hi) {
        const double v = options.value(key, fallback);
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    Settings s;
    s.stepM = bounded("stepM", s.stepM, 10.0, 2000.0);
    s.clearanceM = bounded("clearanceM", s.clearanceM, 0.0, 1000.0);
    s.takeoffM = bounded("takeoffM", s.takeoffM, 0.0, 5000.0);
    s.ceilingAglM = bounded("ceilingAglM", s.ceilingAglM, 10.0, 5000.0);
    s.linkFloor = options.value("linkFloor", s.linkFloor);
    s.fraction = bounded("fresnelFraction", s.fraction, 0.01, 1.0);
    s.maxAglM = bounded("maxAglM", s.maxAglM, 10.0, 5000.0);
    return s;
}

double LegGround::groundAt(double alongM) const {
    if (g_.empty()) throw MissingTerrain("empty terrain profile");
    if (alongM <= g_.front().distanceM) return g_.front().groundMslM;
    const auto next = std::ranges::find_if(g_, [alongM](const GroundSample& x) { return x.distanceM >= alongM; });
    if (next == g_.end()) return g_.back().groundMslM;
    return lerpGround(*std::prev(next), *next, alongM);
}

double LegGround::highestIn(double fromM, double toM) const {
    double top = std::max(groundAt(fromM), groundAt(toM));
    for (const GroundSample& x : g_) {
        if (x.distanceM >= fromM && x.distanceM <= toM) top = std::max(top, x.groundMslM + x.obstacleM);
    }
    return top;
}

std::vector<double> sampleDistances(const Route& route, double stepM) {
    std::vector at{0.0};
    for (std::size_t i = 1; static_cast<double>(i) * stepM < route.length(); ++i) at.push_back(static_cast<double>(i) * stepM);
    for (const Leg& leg : route.legs()) at.push_back(leg.startM + leg.lengthM);
    std::ranges::sort(at);
    const auto duplicates = std::ranges::unique(at, [](double a, double b) { return std::abs(a - b) < 1e-6; });
    at.erase(duplicates.begin(), duplicates.end());
    return at;
}

json analyze(const json& request, const IWorld& world, const LinkInputs& links) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const Settings set = Settings::fromOptions(request.value("options", json::object()));
    const auto [k, kNote] = refraction(request.value("weather", json()));
    json data = {{"kFactor", k},
                 {"notes", {"Sampled every " + fixed(set.stepM, 0) + " m along the route and at every waypoint; the floor takes the highest ground and obstacles within half a step of each sample.",
                            "Within " + fixed(set.takeoffM, 0) + " m of the pilot the floor rises from the ground to the full clearance, and a plan under it there is not warned about.",
                            "Refraction: " + kNote + "."}}};
    json reasons = json::array();
    try {
        const Route route(mission, world);
        const CorridorBuilder builder(mission, route, world, set, links, k);
        const std::vector<CorridorPoint> pts = builder.build();
        data["reference"] = route.altitude().reference() == AltitudeReference::Launch ? "launch" : "agl";
        const double launch = world.groundMsl(mission.pilot.position);
        data["pilot"] = {{"lat", mission.pilot.position.lat}, {"lon", mission.pilot.position.lon},
                         {"groundMslM", launch}, {"antennaMslM", launch + mission.pilot.heightM}};
        data["corridor"] = corridorJson(pts, set);
        data["waypoints"] = waypointsJson(route, world);
        data["summary"] = summaryJson(pts);
        if (const auto* w = widestLink(links)) {
            data["fresnel"] = {{"link", w->first}, {"radio", w->second.radioName}, {"frequencyHz", w->second.frequency.value()}};
        }
        reasons = legReasons(route, pts, set);
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Altitude corridor not drawn: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::altitude
