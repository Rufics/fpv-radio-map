#include "relay.hpp"

#include <algorithm>
#include <optional>

namespace fpv::relay {
namespace {

constexpr const char* kId = "fpv.relay";

json hopsJson(const Hops& h) {
    return {{"pilotToRelay", {{"worstMarginDb", h.first.marginDb}, {"link", h.first.link}}},
            {"relayToDrone", {{"worstMarginDb", h.second.marginDb}, {"link", h.second.link}, {"sample", h.second.sample}}},
            {"scoreDb", h.score()}};
}

/// Both hops through a relay at `relay`, or nothing when the relay spot has no terrain data
/// (such a cell is not a candidate).
std::optional<Hops> relayCandidate(const IWorld& world, const ILinkEvaluator& eval, const std::vector<std::string>& links,
                                   const End& pilot, const End& relay, const std::vector<RoutePoint>& samples) {
    try {
        (void)world.groundMsl(relay.position);
        return evaluateRelay(eval, links, pilot, relay, samples);
    } catch (const MissingTerrain&) {
        return std::nullopt;
    }
}

}  // namespace

Hops evaluateRelay(const ILinkEvaluator& eval, const std::vector<std::string>& links, const End& pilot, const End& relay,
                   const std::vector<RoutePoint>& samples) {
    Hops h;
    for (const std::string& link : links) {
        const double m = eval.marginDb(link, pilot, relay);
        if (m < h.first.marginDb) h.first = {m, link, 0};
    }
    h.second = worstAlongRoute(eval, links, relay, samples);
    return h;
}

json analyze(const json& request, const IWorld& world, const ILinkEvaluator& eval) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    const double relayAgl = options.value("relayAglM", 2.0);
    const double stepM = options.value("stepM", 250.0);
    const double cellM = options.value("cellM", 100.0);
    const auto maxPoints = options.value("maxCandidates", std::size_t{200});
    json reasons = json::array();
    json data = {{"relayAglM", relayAgl}, {"stepM", stepM},
                 {"notes", {"The relay is assumed to use the mission's air radio and antenna on both hops.",
                            "Drone positions are checked every " + fixed(stepM, 0) + " m on the way out and at the target."}}};
    try {
        const Route route(mission, world);
        const std::vector<RoutePoint> samples = routeSamples(route, stepM);
        const std::vector<std::string> links = mission.links();
        const End pilot{mission.pilot.position, mission.pilot.aglM};
        const WorstMargin direct = worstAlongRoute(eval, links, pilot, samples);
        data["direct"] = {{"worstMarginDb", direct.marginDb}, {"link", direct.link}};

        std::optional<Hops> chosen;
        if (options.contains("relay")) {
            const json& r = options.at("relay");
            const End relay{{r.at("lat").get<double>(), r.at("lon").get<double>()}, r.value("aglM", relayAgl)};
            chosen = evaluateRelay(eval, links, pilot, relay, samples);
            data["chosen"] = hopsJson(*chosen);
            data["chosen"]["lat"] = relay.position.lat;
            data["chosen"]["lon"] = relay.position.lon;
            const std::string sev = chosen->score() >= 0.0 ? "info" : "blocked";
            reasons.push_back(reason(kId, sev,
                                     "Relay: pilot → relay worst " + fixed(chosen->first.marginDb, 1) + " dB (" + chosen->first.link +
                                         "), relay → drone worst " + fixed(chosen->second.marginDb, 1) + " dB (" +
                                         chosen->second.link + "); without it " + fixed(direct.marginDb, 1) + " dB."));
        }

        if (!options.contains("area")) {
            if (!chosen) reasons.push_back(reason(kId, "info", "Place a relay or draw an area to search it for a relay spot."));
            return {{"reasons", reasons}, {"data", data}};
        }
        const DrawnArea area = DrawnArea::fromJson(options.at("area"));
        struct Scored {
            LatLon p;
            Hops h;
        };
        std::vector<Scored> scored;
        for (const LatLon& p : area.grid(world, cellM, maxPoints)) {
            if (std::optional<Hops> h = relayCandidate(world, eval, links, pilot, {p, relayAgl}, samples)) {
                scored.emplace_back(p, std::move(*h));
            }
        }
        std::ranges::sort(scored, [](const Scored& a, const Scored& b) { return a.h.score() > b.h.score(); });
        json cells = json::array();
        for (const Scored& s : scored) {
            json c = hopsJson(s.h);
            c["lat"] = s.p.lat;
            c["lon"] = s.p.lon;
            cells.push_back(c);
        }
        data["candidates"] = cells;
        if (scored.empty()) {
            reasons.push_back(reason(kId, "warn", "No point in the area has terrain data."));
        } else {
            const Scored& best = scored.front();
            const double baseline = chosen ? chosen->score() : direct.marginDb;
            if (best.h.score() > baseline) {
                json fix = {{"id", "fpv.relay/place"},
                            {"title", "Put the relay at the best spot"},
                            {"patch", {{"modules", {{kId, {{"relay", {{"lat", best.p.lat}, {"lon", best.p.lon}, {"aglM", relayAgl}}}}}}}}},
                            {"preview", {{{"label", "Worst margin, either hop"},
                                          {"now", fixed(baseline, 1) + " dB" + (chosen ? "" : " (no relay)")},
                                          {"after", fixed(best.h.score(), 1) + " dB"}}}}};
                reasons.push_back(reason(kId, best.h.score() >= 0.0 ? "info" : "warn",
                                         "Best relay spot in the area: worst hop " + fixed(best.h.score(), 1) + " dB.", fix));
            }
        }
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Relay not checked: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::relay
