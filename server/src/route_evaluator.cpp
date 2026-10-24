#include "route_evaluator.hpp"

#include "module_call.hpp"
#include "route_altitude.hpp"
#include "route_preparation.hpp"

#include <rfcore/geodesy.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace rfmap {

namespace {

std::string fmt(double v, int precision = 4) {
    std::ostringstream s;
    s.precision(precision);
    s << v;
    return s.str();
}

std::string km(double m) { return fmt(m / 1000.0, 4) + " km"; }

std::string label(const json& p, const std::string& fallback) {
    const std::string l = p.value("label", "");
    return l.empty() ? fallback : l;
}

/// "blocked" below 0 dB, "warn" when the band says marginal, "info" otherwise.
std::string marginSeverity(double marginDb, std::string_view verdict) {
    if (marginDb < 0.0) return "blocked";
    if (verdict == "marginal") return "warn";
    return "info";
}

/// The reason text for a link's worst sample on a leg.
std::string worstMarginText(LinkKind k, const json& r, double along, const std::string& verdict) {
    std::string text = (k == LinkKind::Control ? "Control" : "Video") + std::string(" link: worst margin ") +
                       fmt(r.at("marginDb").get<double>()) + " dB at " + km(along) + " along the leg";
    if (r.contains("band")) {
        text += " (with terrain error " + fmt(r["band"]["worstDb"].get<double>()) + " to " +
                fmt(r["band"]["bestDb"].get<double>()) + " dB, " + verdict + ")";
    }
    if (r.contains("analogGrade")) text += "; picture " + r["analogGrade"].get<std::string>();
    return text;
}

}  // namespace

RouteEvaluator::RouteEvaluator(LinkService& links, const ModuleRunner& modules) : links_(links), modules_(modules) {}

void RouteEvaluator::recordMissing(LegScan& scan, const PreparedPath& p, double along) const {
    ++scan.missingCount;
    const json md = links_.missingData(p.missing, p.ground.position, p.air.position);
    scan.missingBbox = LinkService::unionBbox(scan.missingBbox, md.at("missingData").at("bbox"));
    if (scan.missing.size() < 20) {
        scan.missing.push_back({{"alongLegM", along},
                                {"lat", p.air.position.latitudeDeg},
                                {"lon", p.air.position.longitudeDeg},
                                {"missing", md.at("missing")}});
    }
}

void RouteEvaluator::recordWorst(LegScan& scan, const PreparedPath& p, double along, const Links& links,
                                 const CalcContext& ctx, std::vector<ModuleFailure>& failures) const {
    for (const auto& [k, l] : links) {
        const LinkEvaluation e = links_.evaluate(p, l, ctx, false, false);
        failures.insert(failures.end(), e.failures.begin(), e.failures.end());
        WorstSample& w = scan.worst[k];
        if (e.marginDb < w.margin) {
            w.margin = e.marginDb;
            w.path = p;
            w.along = along;
        }
    }
}

json RouteEvaluator::missingDataLeg(json leg, const LegScan& scan, const DemSource& dem, std::size_t total) {
    json reasons = json::array();
    leg["status"] = "missing-data";
    leg["missing"] = scan.missing;
    leg["missingData"] = {{"bbox", scan.missingBbox}, {"source", dem.name()}};
    reasons.push_back({{"module", "rf-core"},
                       {"severity", "blocked"},
                       {"text", std::format("No terrain data for {} of {} positions on this leg; the leg is not "
                                            "evaluated and no heights are guessed.",
                                            scan.missingCount, total)}});
    leg["reasons"] = reasons;
    return leg;
}

void RouteEvaluator::describeWorst(json& leg, const LegScan& scan, const Links& links, const CalcContext& ctx,
                                   std::vector<ModuleFailure>& failures) const {
    std::string status = "ok";
    json reasons = json::array();
    json worstJson = json::object();
    for (const auto& [k, l] : links) {
        const auto found = scan.worst.find(k);
        if (found == scan.worst.end()) continue;
        const WorstSample& w = found->second;
        const std::optional<PreparedPath>& path = w.path;
        if (!path.has_value()) continue;
        LinkEvaluation e = links_.evaluate(*path, l, ctx, true, true);
        failures.insert(failures.end(), e.failures.begin(), e.failures.end());
        json& r = e.result;
        r["sample"] = {{"alongLegM", w.along},
                       {"lat", path->air.position.latitudeDeg},
                       {"lon", path->air.position.longitudeDeg},
                       {"aglM", path->air.aglM}};
        const std::string verdict = r.contains("band") ? r["band"].value("verdict", "") : "";
        const std::string severity = marginSeverity(r.at("marginDb").get<double>(), verdict);
        if (severity == "blocked") status = "blocked";
        else if (severity == "warn" && status == "ok") status = "warn";
        reasons.push_back({{"module", "rf-core"},
                           {"severity", severity},
                           {"text", worstMarginText(k, r, w.along, verdict)},
                           {"link", toString(k)}});
        worstJson[toString(k)] = std::move(r);
    }
    leg["worst"] = worstJson;
    leg["status"] = status;
    leg["reasons"] = reasons;
}

void RouteEvaluator::scanSample(LegScan& scan, const Terminal& pilot, const Terminal& drone, double along,
                                const LegContext& lc) const {
    if (!(rfcore::Geodesy().inverse(pilot.position, drone.position).distance.value() > 0.0)) {
        ++scan.overPilot;  // the drone is over the pilot: there is no path to evaluate
        return;
    }
    PreparedPath p = links_.prepare(pilot, drone, lc.ctx);
    if (!p.complete()) {
        recordMissing(scan, p, along);
        return;
    }
    ++scan.evaluated;
    recordWorst(scan, p, along, lc.links, lc.ctx, lc.failures);
}

void RouteEvaluator::recordBelowGround(LegScan& scan, double aglM, double along) {
    ++scan.belowGround;
    if (aglM < scan.lowestAglM) {
        scan.lowestAglM = aglM;
        scan.lowestAlongM = along;
    }
}

json RouteEvaluator::evaluateLeg(const json& mission, std::size_t index, const json& from, const json& to,
                                 const Links& links, const CalcContext& ctx, std::vector<ModuleFailure>& failures) const {
    const DemSource& dem = links_.requireDem();
    const rfcore::Geodesy geo;
    const RouteAltitude altitude(mission, dem);
    const std::size_t n = mission.at("waypoints").size();
    const Terminal pilot = Terminal::fromJson(mission.at("pilot"));
    const rfcore::GeoPoint a{from.at("lat").get<double>(), from.at("lon").get<double>()};
    const rfcore::GeoPoint b{to.at("lat").get<double>(), to.at("lon").get<double>()};
    const double heightA = altitude.heightOf(from, index == 0);
    const double heightB = altitude.heightOf(to, index == n);
    const double length = geo.inverse(a, b).distance.value();

    json leg = {{"index", index}, {"lengthM", length}};
    leg["from"] = index == 0 ? label(from, "Pilot") : label(from, std::format("WP{}", index));
    leg["to"] = index < n ? label(to, std::format("WP{}", index + 1)) : label(to, "Pilot");

    // Sample positions: the leg's geodesic at the DEM's native step, both ends included. The
    // drone's height changes linearly with distance along the leg in the mission's altitude
    // reference (route_altitude.hpp).
    std::vector<rfcore::GeoPoint> pts;
    if (length > 0.0) pts = geo.sample(a, b, dem.nativeResolution());
    else pts = {a};
    const double steps = std::max<double>(1.0, static_cast<double>(pts.size() - 1));

    LegScan scan;
    const LegContext lc{links, ctx, failures};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const double f = static_cast<double>(i) / steps;
        // Without ground under the drone the path is incomplete and recorded as missing data.
        const double agl = altitude.aglAt(pts[i], std::lerp(heightA, heightB, f)).value_or(0.0);
        if (agl < 0.0) recordBelowGround(scan, agl, length * f);
        scanSample(scan, pilot, Terminal{pts[i], std::max(0.0, agl)}, length * f, lc);
    }
    leg["samples"] = {{"total", pts.size()}, {"evaluated", scan.evaluated}, {"overPilot", scan.overPilot},
                      {"missingData", scan.missingCount}, {"belowGround", scan.belowGround},
                      {"stepM", dem.nativeResolution().value()}};
    if (scan.missingCount > 0) return missingDataLeg(std::move(leg), scan, dem, pts.size());
    describeWorst(leg, scan, links, ctx, failures);
    if (scan.belowGround > 0) markBelowGround(leg, scan, pts.size());
    return leg;
}

void RouteEvaluator::markBelowGround(json& leg, const LegScan& scan, std::size_t total) {
    leg["status"] = "blocked";
    leg["reasons"].push_back({{"module", "rf-core"},
                              {"severity", "blocked"},
                              {"text", std::format("The planned altitude is below the ground at {} of {} positions on this "
                                                   "leg, down to {:.1f} m below it at {} along the leg. Links there are "
                                                   "evaluated with the drone on the ground.",
                                                   scan.belowGround, total, -scan.lowestAglM,
                                                   km(scan.lowestAlongM))}});
}

std::optional<json> RouteEvaluator::analyze(const ModuleRunner::Active& a, const json& req, json& result) {
    json resp;
    std::string failure = ModuleCall::failure([&a, &req, &resp] { resp = a.instance->invoke("route.analyze", req); });
    if (failure.empty() && !resp.is_object()) failure = "route.analyze did not return an object";
    if (failure.empty()) return std::make_optional(std::move(resp));
    ModuleFailure{a.id, failure}.appendTo(result["moduleFailures"]);
    return std::nullopt;
}

void RouteEvaluator::addReason(json& result, json r, const std::string& moduleId) {
    r["module"] = moduleId;
    if (const std::string sev = r.value("severity", "info"); sev != "info" && sev != "warn" && sev != "blocked") {
        r["severity"] = "info";
    }
    if (r.contains("leg") && r["leg"].is_number_integer()) {
        if (const auto i = r["leg"].get<long long>(); i >= 0 && static_cast<std::size_t>(i) < result["legs"].size()) {
            result["legs"][static_cast<std::size_t>(i)]["reasons"].push_back(r);
            return;
        }
    }
    result["reasons"].push_back(r);
}

void RouteEvaluator::runModules(const json& mission, json& result, const CalcContext& ctx) const {
    json soFar = json::object();
    for (const ModuleRunner::Active& a : modules_.optedIn("route.analysis", ctx)) {
        const json req = {{"mission", mission},
                          {"legs", result["legs"]},
                          {"options", a.options},
                          {"modules", soFar},
                          {"weather", ctx.forecast.has_value() ? ctx.forecast->point : json(nullptr)},
                          {"time", UtcTime::format(ctx.time)}};
        const std::optional<json> answer = analyze(a, req, result);
        if (!answer.has_value()) continue;
        const json& resp = *answer;
        for (const json& r : resp.value("reasons", json::array())) {
            if (r.is_object()) addReason(result, r, a.id);
        }
        const json data = resp.value("data", json::object());
        result["modules"][a.id] = data;
        soFar[a.id] = data;
        if (resp.contains("battery") && !result.contains("battery")) result["battery"] = resp.at("battery");
    }
}

void RouteEvaluator::evaluate(const json& request, const Links& links, const CalcContext& ctx, json& result) const {
    json early = json::array();
    const json mission = RoutePreparation(modules_).prepare(request, ctx, result, early);
    // Legs: pilot -> wp1 -> ... -> last, and back to the pilot when returnToPilot (default true).
    std::vector<json> points{mission.at("pilot")};
    for (const json& w : mission.at("waypoints")) points.push_back(w);
    if (mission.value("returnToPilot", true)) points.push_back(mission.at("pilot"));
    std::vector<ModuleFailure> failures;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        result["legs"].push_back(evaluateLeg(mission, i, points[i], points[i + 1], links, ctx, failures));
    }
    ModuleFailure::appendAll(result["moduleFailures"], failures);
    for (const json& r : early) addReason(result, r, r.at("module").get<std::string>());
    runModules(mission, result, ctx);
}

}  // namespace rfmap
