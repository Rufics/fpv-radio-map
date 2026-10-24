#include "route_preparation.hpp"

#include "module_call.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace rfmap {

std::string RoutePreparation::heightField(const json& mission) {
    return mission.value("altitudeRef", "agl") == "launch" ? "altM" : "aglM";
}

bool RoutePreparation::wantsHeight(const json& waypoint, const std::string& field) {
    return !waypoint.contains(field) || waypoint.value("heightSource", "manual") == "auto";
}

bool RoutePreparation::apply(json& mission, const json& answer, const std::string& moduleId, json& heights,
                             std::vector<bool>& done) {
    const std::string field = heightField(mission);
    json& waypoints = mission["waypoints"];
    for (const json& h : answer.value("waypoints", json::array())) {
        if (!h.is_object() || !h.contains("index") || !h["index"].is_number_unsigned() || !h.contains("heightM") ||
            !h["heightM"].is_number() || (h.contains("field") && h["field"] != field) ||
            (h.contains("blocked") && !h["blocked"].is_boolean())) {
            return false;
        }
        const auto i = h["index"].get<std::size_t>();
        const double height = h["heightM"].get<double>();
        if (i >= waypoints.size() || !std::isfinite(height)) return false;
        if (done[i] || !wantsHeight(waypoints[i], field)) continue;  // manual, or set by an earlier module
        done[i] = true;
        waypoints[i][field] = height;
        waypoints[i]["heightSource"] = "auto";
        // Where the waypoint was, so a client can tell the entry still belongs to it after an edit.
        json entry = {{"index", i}, {"lat", waypoints[i].at("lat")}, {"lon", waypoints[i].at("lon")}, {"field", field},
                      {"heightM", height}, {"module", moduleId}, {"blocked", h.value("blocked", false)}};
        if (h.contains("limitedBy") && h["limitedBy"].is_string()) entry["limitedBy"] = h["limitedBy"];
        heights.push_back(std::move(entry));
    }
    return true;
}

json RoutePreparation::prepare(const json& mission, const CalcContext& ctx, json& result, json& reasons) const {
    const std::string field = heightField(mission);
    json prepared = mission;
    const json& wps = mission.at("waypoints");
    if (std::ranges::none_of(wps, [&field](const json& w) { return wantsHeight(w, field); })) return prepared;
    std::vector<bool> done(wps.size(), false);
    for (const ModuleRunner::Active& a : modules_.optedIn("route.preparation", ctx)) {
        const json req = {{"mission", prepared},
                          {"options", a.options},
                          {"weather", ctx.forecast.has_value() ? ctx.forecast->point : json(nullptr)},
                          {"time", UtcTime::format(ctx.time)}};
        json resp;
        std::string failure = ModuleCall::failure([&a, &req, &resp] { resp = a.instance->invoke("route.prepare", req); });
        if (failure.empty() && !resp.is_object()) failure = "route.prepare did not return an object";
        // A malformed answer sets nothing: what it did set is dropped with the copies.
        json attempt = prepared;
        json heights = json::array();
        std::vector<bool> set = done;
        if (failure.empty() && !apply(attempt, resp, a.id, heights, set)) failure = "route.prepare returned a malformed waypoint height";
        if (!failure.empty()) {
            ModuleFailure{a.id, failure}.appendTo(result["moduleFailures"]);
            continue;
        }
        prepared = std::move(attempt);
        done = std::move(set);
        for (json& h : heights) result["autoHeights"].push_back(std::move(h));
        for (json r : resp.value("reasons", json::array())) {
            if (!r.is_object()) continue;
            r["module"] = a.id;
            reasons.push_back(std::move(r));
        }
    }
    const json& waypoints = prepared.at("waypoints");
    for (std::size_t i = 0; i < waypoints.size(); ++i) {
        if (waypoints[i].contains(field)) continue;
        throw HttpError(422, std::format("waypoints[{}] has no {}: set its height, or turn on fpv.altitude for automatic heights", i, field));
    }
    return prepared;
}

}  // namespace rfmap
