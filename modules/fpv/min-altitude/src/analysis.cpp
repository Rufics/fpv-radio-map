#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
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
    std::vector<double> legNeed;
    std::vector<bool> legUnreachable;
};

/// Effective earth factor and how it was chosen: the forecast's when present, else 4/3.
std::pair<double, std::string> refraction(const json& weather) {
    if (weather.is_object() && weather.contains("effectiveEarthFactor") && weather.at("effectiveEarthFactor").is_number()) {
        const double k = weather.at("effectiveEarthFactor").get<double>();
        return {k, "k = " + fixed(k, 3) + " from the forecast refractivity gradient"};
    }
    return {4.0 / 3.0, "standard atmosphere, k = 4/3 (no forecast refraction for this time)"};
}

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
        samples.push_back({s, p});
    }
    return samples;
}

LinkResult requiredAlongRoute(const Route& route, const Mission& mission, const IWorld& world,
                              const std::vector<Sample>& samples, const ClearanceSolver& solver, double maxAgl) {
    LinkResult r;
    r.legNeed.assign(route.legs().size(), 0.0);
    r.legUnreachable.assign(route.legs().size(), false);
    for (const Sample& smp : samples) {
        const InverseResult inv = world.inverse(mission.pilot.position, smp.p.position);
        const DirectResult mid = world.direct(mission.pilot.position, inv.azimuth1Deg, inv.distanceM / 2.0);
        const auto profile = ClearanceSolver::toProfile(world.groundProfile(mission.pilot.position, smp.p.position),
                                                        mid.azimuthDeg, mid.point.lat);
        const auto need = solver.requiredAgl(profile, mission.pilot.aglM, maxAgl);
        json pt = {{"distanceAlongRouteM", smp.s}, {"lat", smp.p.position.lat}, {"lon", smp.p.position.lon},
                   {"leg", smp.p.leg}, {"plannedAglM", smp.p.aglM}};
        if (need.has_value()) {
            pt["requiredAglM"] = *need;
            r.legNeed[smp.p.leg] = std::max(r.legNeed[smp.p.leg], *need);
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

/// Raising both ends of a leg to the leg's maximum keeps every sample clear,
/// because the AGL is interpolated between them.
json raiseLegFix(const json& request, const Leg& leg, const std::string& legName, double planned, double need) {
    json wps = request.at("mission").at("waypoints");
    const std::size_t toIdx = leg.index;          // waypoint index of leg.to
    if (toIdx < wps.size()) wps[toIdx]["aglM"] = std::max(wps[toIdx].at("aglM").get<double>(), need);
    if (leg.index >= 1) wps[leg.index - 1]["aglM"] = std::max(wps[leg.index - 1].at("aglM").get<double>(), need);
    return {{"id", std::format("{}/raise-leg-{}", kId, leg.index)},
            {"title", "Raise " + legName + " to " + fixed(need, 1) + " m"},
            {"patch", {{"waypoints", wps}}},
            {"preview", {{{"label", "Height on " + legName}, {"now", fixed(planned, 1) + " m AGL"}, {"after", fixed(need, 1) + " m AGL"}}}}};
}

/// The reason for one leg, or nothing when the planned height already clears it.
std::optional<json> legReason(const json& request, const Leg& leg, const std::vector<Sample>& samples,
                              const LinkResult& r, const Settings& set) {
    const std::string legName = leg.from.label + " → " + leg.to.label;
    if (r.legUnreachable[leg.index]) {
        return reason(kId, "blocked",
                      "Leg " + legName + ": no height up to " + fixed(set.maxAgl, 0) + " m AGL clears " +
                          fixed(set.fraction * 100.0, 0) + " % of the " + set.kind + " Fresnel zone.");
    }
    const double planned = lowestPlanned(samples, leg.index);
    if (!anyBelowRequired(r.points, leg.index)) return std::nullopt;
    const double need = std::ceil(r.legNeed[leg.index] * 10.0) / 10.0;  // display and patch at 0.1 m, rounded up
    const json fix = leg.isReturn ? json(nullptr) : raiseLegFix(request, leg, legName, planned, need);
    return reason(kId, "warn",
                  "Leg " + legName + ": " + fixed(set.fraction * 100.0, 0) + " % of the " + set.kind +
                      " Fresnel zone is clear only from " + fixed(need, 1) + " m AGL; planned " +
                      fixed(planned, 1) + " m.",
                  fix);
}

}  // namespace

json analyze(const json& request, const IWorld& world, const std::map<std::string, LinkInput>& links) {
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
                if (auto legWhy = legReason(request, leg, samples, r, set)) reasons.push_back(*legWhy);
            }
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Minimum height not checked: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::minalt
