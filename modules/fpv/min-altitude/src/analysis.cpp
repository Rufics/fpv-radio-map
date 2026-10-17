#include "analysis.hpp"

#include <algorithm>
#include <cmath>

namespace fpv::minalt {
namespace {

constexpr const char* kId = "fpv.min-altitude";

struct Sample {
    double s;
    RoutePoint p;
};

}  // namespace

json analyze(const json& request, const IWorld& world, const std::map<std::string, LinkInput>& links) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    const double fraction = options.value("clearanceFraction", 0.6);
    const double stepM = options.value("stepM", 100.0);
    const double maxAgl = options.value("maxAglM", 500.0);
    const json weather = request.value("weather", json());
    double k = 4.0 / 3.0;
    std::string kNote = "standard atmosphere, k = 4/3 (no forecast refraction for this time)";
    if (weather.is_object() && weather.contains("effectiveEarthFactor") && weather.at("effectiveEarthFactor").is_number()) {
        k = weather.at("effectiveEarthFactor").get<double>();
        kNote = "k = " + fixed(k, 3) + " from the forecast refractivity gradient";
    }

    json reasons = json::array();
    json data = {{"clearanceFraction", fraction}, {"stepM", stepM}, {"kFactor", k}, {"links", json::object()},
                 {"notes", {"Checked every " + fixed(stepM, 0) + " m along the route and at every waypoint; points in between are not checked.",
                            "Refraction: " + kNote + "."}}};
    try {
        const Route route(mission, world);
        // Sample points: regular spacing plus every leg end, skipping the pilot's own position.
        std::vector<double> at;
        for (double s = stepM; s < route.length(); s += stepM) at.push_back(s);
        for (const Leg& leg : route.legs()) at.push_back(leg.startM + leg.lengthM);
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), at.end());
        std::vector<Sample> samples;
        for (double s : at) {
            const RoutePoint p = route.at(s);
            if (world.inverse(mission.pilot.position, p.position).distanceM < 1.0) continue;
            samples.push_back({s, p});
        }

        for (const auto& [kind, link] : links) {
            const ClearanceSolver solver(k, link.frequency, fraction);
            json points = json::array();
            std::vector<double> legNeed(route.legs().size(), 0.0);
            std::vector<bool> legUnreachable(route.legs().size(), false);
            for (const Sample& smp : samples) {
                const InverseResult inv = world.inverse(mission.pilot.position, smp.p.position);
                const DirectResult mid = world.direct(mission.pilot.position, inv.azimuth1Deg, inv.distanceM / 2.0);
                const auto profile = ClearanceSolver::toProfile(world.groundProfile(mission.pilot.position, smp.p.position),
                                                                mid.azimuthDeg, mid.point.lat);
                const auto need = solver.requiredAgl(profile, mission.pilot.aglM, maxAgl);
                json pt = {{"distanceAlongRouteM", smp.s}, {"lat", smp.p.position.lat}, {"lon", smp.p.position.lon},
                           {"leg", smp.p.leg}, {"plannedAglM", smp.p.aglM}};
                if (need) {
                    pt["requiredAglM"] = *need;
                    legNeed[smp.p.leg] = std::max(legNeed[smp.p.leg], *need);
                } else {
                    pt["requiredAglM"] = nullptr;
                    legUnreachable[smp.p.leg] = true;
                }
                points.push_back(pt);
            }
            data["links"][kind] = {{"radio", link.radioName}, {"frequencyHz", link.frequency.value()}, {"points", points}};

            for (const Leg& leg : route.legs()) {
                const std::string legName = leg.from.label + " → " + leg.to.label;
                if (legUnreachable[leg.index]) {
                    reasons.push_back(reason(kId, "blocked",
                                             "Leg " + legName + ": no height up to " + fixed(maxAgl, 0) + " m AGL clears " +
                                                 fixed(fraction * 100.0, 0) + " % of the " + kind + " Fresnel zone."));
                    continue;
                }
                double planned = 1e300;
                for (const Sample& smp : samples) {
                    if (smp.p.leg == leg.index) planned = std::min(planned, smp.p.aglM);
                }
                bool low = false;
                for (const json& pt : points) {
                    if (pt.at("leg").get<std::size_t>() == leg.index && pt.at("plannedAglM").get<double>() < pt.at("requiredAglM").get<double>()) low = true;
                }
                if (!low) continue;
                const double need = std::ceil(legNeed[leg.index] * 10.0) / 10.0;  // display and patch at 0.1 m, rounded up
                json fix = nullptr;
                if (!leg.isReturn) {
                    // Raising both ends of a leg to the leg's maximum keeps every sample clear,
                    // because the AGL is interpolated between them.
                    json wps = request.at("mission").at("waypoints");
                    const std::size_t toIdx = leg.index;          // waypoint index of leg.to
                    if (toIdx < wps.size()) wps[toIdx]["aglM"] = std::max(wps[toIdx].at("aglM").get<double>(), need);
                    if (leg.index >= 1) wps[leg.index - 1]["aglM"] = std::max(wps[leg.index - 1].at("aglM").get<double>(), need);
                    fix = {{"id", std::string(kId) + "/raise-leg-" + std::to_string(leg.index)},
                           {"title", "Raise " + legName + " to " + fixed(need, 1) + " m"},
                           {"patch", {{"waypoints", wps}}},
                           {"preview", {{{"label", "Height on " + legName}, {"now", fixed(planned, 1) + " m AGL"}, {"after", fixed(need, 1) + " m AGL"}}}}};
                }
                reasons.push_back(reason(kId, "warn",
                                         "Leg " + legName + ": " + fixed(fraction * 100.0, 0) + " % of the " + kind +
                                             " Fresnel zone is clear only from " + fixed(need, 1) + " m AGL; planned " +
                                             fixed(planned, 1) + " m.",
                                         fix));
            }
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Minimum height not checked: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::minalt
