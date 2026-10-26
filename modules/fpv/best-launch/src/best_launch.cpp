#include "best_launch.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace fpv::launch {
namespace {
constexpr const char* kId = "fpv.best-launch";

/// Worst margin along the route when launching from `ground`, or nothing when the launch
/// point has no terrain data.
std::optional<WorstMargin> worstFrom(const IWorld& world, const ILinkEvaluator& eval, const std::vector<std::string>& links,
                                     const End& ground, const std::vector<RoutePoint>& samples) {
    try {
        (void)world.groundMsl(ground.position);
        return worstAlongRoute(eval, links, ground, samples);
    } catch (const MissingTerrain&) {
        return std::nullopt;
    }
}
}  // namespace

json analyze(const json& request, const IWorld& world, const ILinkEvaluator& eval) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    if (mission.links().empty()) {
        return {{"reasons", {reason(kId, "info", "The best launch point is scored by link margin; evaluate at least one link to search for it.")}}, {"data", json::object()}};
    }
    if (!options.contains("area")) {
        return {{"reasons", {reason(kId, "info", "Draw an area to search it for the best launch point.")}}, {"data", json::object()}};
    }
    const double cellM = options.value("cellM", 100.0);
    const double stepM = options.value("stepM", 250.0);
    const auto maxPoints = options.value("maxCandidates", std::size_t{200});
    const double agl = options.value("antennaAglM", mission.pilot.heightM);
    json reasons = json::array();
    json data = {{"cellM", cellM}, {"stepM", stepM}, {"antennaAglM", agl}};
    try {
        const DrawnArea area = DrawnArea::fromJson(options.at("area"));
        const std::vector<LatLon> grid = area.grid(world, cellM, maxPoints);
        const Route route(mission, world);
        const std::vector<RoutePoint> samples = routeSamples(route, stepM);
        const std::vector<std::string> links = mission.links();

        const WorstMargin now = worstAlongRoute(eval, links, {mission.pilot.position, mission.pilot.heightM}, samples);
        struct Scored {
            LatLon p;
            WorstMargin w;
        };
        std::vector<Scored> scored;
        std::size_t noData = 0;
        for (const LatLon& p : grid) {
            if (std::optional<WorstMargin> w = worstFrom(world, eval, links, {p, agl}, samples)) {
                scored.emplace_back(p, std::move(*w));
            } else {
                ++noData;
            }
        }
        std::ranges::sort(scored, [](const Scored& a, const Scored& b) { return a.w.marginDb > b.w.marginDb; });

        json cells = json::array();
        for (const Scored& s : scored) {
            cells.push_back({{"lat", s.p.lat}, {"lon", s.p.lon}, {"worstMarginDb", s.w.marginDb}, {"link", s.w.link}});
        }
        data["candidates"] = cells;
        data["current"] = {{"worstMarginDb", now.marginDb}, {"link", now.link}};
        data["notes"] = {"Each candidate is scored by its worst margin over every evaluated link at points every " +
                         fixed(stepM, 0) + " m along the way out and at the target; points in between are not checked."};
        if (noData > 0) data["notes"].push_back(std::format("{} grid points have no terrain data and were skipped.", noData));

        if (scored.empty()) {
            reasons.push_back(reason(kId, "warn", "No launch point in the area has terrain data."));
        } else if (scored.front().w.marginDb > now.marginDb) {
            const Scored& best = scored.front();
            const double gain = best.w.marginDb - now.marginDb;
            json fix = {{"id", "fpv.best-launch/move-pilot"},
                        {"title", "Launch from the best point"},
                        {"patch", {{"pilot", {{"lat", best.p.lat}, {"lon", best.p.lon}, {"aglM", agl}, {"label", mission.pilot.label}}}}},
                        {"preview", {{{"label", "Worst margin on the route"},
                                      {"now", fixed(now.marginDb, 1) + " dB (" + now.link + ")"},
                                      {"after", fixed(best.w.marginDb, 1) + " dB (" + best.w.link + ")"}},
                                     {{"label", "Moves the pilot by"},
                                      {"now", "0 m"},
                                      {"after", fixed(world.inverse(mission.pilot.position, best.p).distanceM, 0) + " m"}}}}};
            reasons.push_back(reason(kId, best.w.marginDb >= 0.0 ? "info" : "warn",
                                     "Best launch point in the area raises the worst margin by " + fixed(gain, 1) + " dB to " +
                                         fixed(best.w.marginDb, 1) + " dB.",
                                     fix));
        } else {
            reasons.push_back(reason(kId, "info", "No point in the area beats the current launch point (worst margin " +
                                                      fixed(now.marginDb, 1) + " dB)."));
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Launch search not run: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::launch
