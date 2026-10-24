#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace fpv::minalt {
namespace {

constexpr const char* kId = "fpv.min-altitude";

struct Sample {
    double s;
    RoutePoint p;
};

/// Request settings the per-leg reasons are worded with.
struct Settings {
    double fraction;
    double maxAgl;
    std::string kind;  ///< link kind of the leg being described
};

/// Required AGL at every sample for one link, and its maximum per leg.
struct LinkResult {
    json points = json::array();
    std::vector<double> legNeed;        ///< highest required AGL on the leg
    std::vector<double> legNeedHeight;  ///< the same in the mission's altitude reference
    std::vector<bool> legUnreachable;
};

/// Sample points: regular spacing plus every leg end, skipping the pilot's own position.
std::vector<Sample> samplePoints(const Route& route, const Mission& mission, const IWorld& world, double stepM) {
    std::vector<double> at;
    for (std::size_t i = 1; static_cast<double>(i) * stepM < route.length(); ++i) at.push_back(static_cast<double>(i) * stepM);
    for (const Leg& leg : route.legs()) at.push_back(leg.startM + leg.lengthM);
    std::ranges::sort(at);
    const auto duplicates = std::ranges::unique(at, [](double a, double b) { return std::abs(a - b) < 1e-6; });
    at.erase(duplicates.begin(), duplicates.end());
    std::vector<Sample> samples;
    for (const double s : at) {
        const RoutePoint p = route.at(s);
        if (world.inverse(mission.pilot.position, p.position).distanceM < 1.0) continue;
        samples.emplace_back(s, p);
    }
    return samples;
}

LinkResult requiredAlongRoute(const Route& route, const Mission& mission, const IWorld& world,
                              const std::vector<Sample>& samples, const ClearanceSolver& solver, double maxAgl) {
    LinkResult r;
    r.legNeed.assign(route.legs().size(), 0.0);
    r.legNeedHeight.assign(route.legs().size(), -1e300);
    r.legUnreachable.assign(route.legs().size(), false);
    for (const Sample& smp : samples) {
        const InverseResult inv = world.inverse(mission.pilot.position, smp.p.position);
        const DirectResult mid = world.direct(mission.pilot.position, inv.azimuth1Deg, inv.distanceM / 2.0);
        const auto profile = ClearanceSolver::toProfile(world.groundProfile(mission.pilot.position, smp.p.position),
                                                        mid.azimuthDeg, mid.point.lat);
        const auto need = solver.requiredAgl(profile, mission.pilot.heightM, maxAgl);
        json pt = {{"distanceAlongRouteM", smp.s}, {"lat", smp.p.position.lat}, {"lon", smp.p.position.lon},
                   {"leg", smp.p.leg}, {"plannedAglM", smp.p.aglM}};
        if (need.has_value()) {
            pt["requiredAglM"] = *need;
            r.legNeed[smp.p.leg] = std::max(r.legNeed[smp.p.leg], *need);
            // The ground under the sample is the same in both references, so the height in the
            // mission's reference moves one for one with the AGL.
            r.legNeedHeight[smp.p.leg] = std::max(r.legNeedHeight[smp.p.leg], smp.p.heightM + *need - smp.p.aglM);
        } else {
            pt["requiredAglM"] = nullptr;
            r.legUnreachable[smp.p.leg] = true;
        }
        r.points.push_back(pt);
    }
    return r;
}

double lowestPlanned(const std::vector<Sample>& samples, std::size_t leg) {
    double planned = 1e300;
    for (const Sample& smp : samples) {
        if (smp.p.leg == leg) planned = std::min(planned, smp.p.aglM);
    }
    return planned;
}

bool anyBelowRequired(const json& points, std::size_t leg) {
    bool low = false;
    for (const json& pt : points) {
        if (pt.at("leg").get<std::size_t>() == leg && pt.at("plannedAglM").get<double>() < pt.at("requiredAglM").get<double>()) low = true;
    }
    return low;
}

/// What a raise-leg fix changes: the leg's lowest planned AGL and the AGL it needs, and the end
/// height in the mission's reference that gives it.
struct Raise {
    double plannedAgl;
    double needAgl;
    double height;
};

/// Raising both ends of a leg to the leg's highest required height keeps every sample clear,
/// because the height is interpolated between them in the mission's reference.
json raiseLegFix(const json& request, const Leg& leg, const std::string& legName, const Raise& r, std::string_view field) {
    json wps = request.at("mission").at("waypoints");
    const std::string key(field);
    auto raise = [&wps, &key, &r](std::size_t i) { wps[i][key] = std::max(wps[i].at(key).get<double>(), r.height); };
    // leg.index is also the waypoint index of leg.to.
    if (leg.index < wps.size()) raise(leg.index);
    if (leg.index >= 1) raise(leg.index - 1);
    const std::string title = field == "aglM" ? fixed(r.height, 1) + " m" : fixed(r.height, 1) + " m above launch";
    return {{"id", std::format("{}/raise-leg-{}", kId, leg.index)},
            {"title", "Raise " + legName + " to " + title},
            {"patch", {{"waypoints", wps}}},
            {"preview", {{{"label", "Height on " + legName}, {"now", fixed(r.plannedAgl, 1) + " m AGL"}, {"after", fixed(r.needAgl, 1) + " m AGL"}}}}};
}

/// The reason for one leg, or nothing when the planned height already clears it.
std::optional<json> legReason(const json& request, const Route& route, const Leg& leg, const std::vector<Sample>& samples,
                              const LinkResult& r, const Settings& set) {
    const std::string legName = leg.from.label + " → " + leg.to.label;
    if (r.legUnreachable[leg.index]) {
        return std::make_optional(reason(kId, "blocked",
                                         "Leg " + legName + ": no height up to " + fixed(set.maxAgl, 0) + " m AGL clears " +
                                             fixed(set.fraction * 100.0, 0) + " % of the " + set.kind + " Fresnel zone."));
    }
    const double planned = lowestPlanned(samples, leg.index);
    if (!anyBelowRequired(r.points, leg.index)) return std::nullopt;
    // Display and patch at 0.1 m, rounded up.
    const Raise raise{planned, std::ceil(r.legNeed[leg.index] * 10.0) / 10.0, std::ceil(r.legNeedHeight[leg.index] * 10.0) / 10.0};
    const double need = raise.needAgl;
    const json fix = leg.isReturn ? json(nullptr) : raiseLegFix(request, leg, legName, raise, route.altitude().waypointField());
    return std::make_optional(reason(kId, "warn",
                                     "Leg " + legName + ": " + fixed(set.fraction * 100.0, 0) + " % of the " + set.kind +
                                         " Fresnel zone is clear only from " + fixed(need, 1) + " m AGL; planned " +
                                         fixed(planned, 1) + " m.",
                                     fix));
}

}  // namespace

json analyze(const json& request, const IWorld& world, const LinkInputs& links) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    Settings set{options.value("clearanceFraction", 0.6), options.value("maxAglM", 500.0), {}};
    const double stepM = options.value("stepM", 100.0);
    const auto [k, kNote] = refraction(request.value("weather", json()));

    json reasons = json::array();
    json data = {{"clearanceFraction", set.fraction}, {"stepM", stepM}, {"kFactor", k}, {"links", json::object()},
                 {"notes", {"Checked every " + fixed(stepM, 0) + " m along the route and at every waypoint; points in between are not checked.",
                            "Refraction: " + kNote + "."}}};
    try {
        const Route route(mission, world);
        const std::vector<Sample> samples = samplePoints(route, mission, world, stepM);
        for (const auto& [kind, link] : links) {
            const ClearanceSolver solver(k, link.frequency, set.fraction);
            const LinkResult r = requiredAlongRoute(route, mission, world, samples, solver, set.maxAgl);
            data["links"][kind] = {{"radio", link.radioName}, {"frequencyHz", link.frequency.value()}, {"points", r.points}};
            set.kind = kind;
            for (const Leg& leg : route.legs()) {
                if (auto legWhy = legReason(request, route, leg, samples, r, set)) reasons.push_back(*legWhy);
            }
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Minimum height not checked: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::minalt
