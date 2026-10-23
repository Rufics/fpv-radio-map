#include "rfflightlog/plan_comparison.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace rfflightlog {
namespace {

std::string fixed(double v, int digits) { return std::format("{:.{}f}", v, digits); }

std::string km(double m) { return fixed(m / 1000.0, 2) + " km"; }

std::string pointLabel(const json& p, const std::string& fallback) {
    return p.contains("label") && p.at("label").is_string() && !p.at("label").get<std::string>().empty()
               ? p.at("label").get<std::string>()
               : fallback;
}

std::optional<double> number(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_number()) return std::nullopt;
    return j.at(key).get<double>();
}

std::optional<double> marginOf(const json& leg, const char* link) {
    if (!leg.contains("worst") || !leg.at("worst").is_object() || !leg.at("worst").contains(link)) return std::nullopt;
    return number(leg.at("worst").at(link), "marginDb");
}

json optionalJson(const std::optional<double>& v) { return v ? json(std::round(*v * 1000.0) / 1000.0) : json(nullptr); }

json row(const char* metric, const char* label, const char* unit, const std::optional<double>& plan,
         const std::optional<double>& fact) {
    json r = {{"metric", metric}, {"label", label}, {"unit", unit}, {"plan", optionalJson(plan)}, {"fact", optionalJson(fact)}};
    if (plan.has_value() && fact.has_value() && *plan != 0.0) r["deltaPct"] = std::round((*fact - *plan) / *plan * 1000.0) / 10.0;
    return r;
}

json finding(const char* severity, std::string text) { return {{"severity", severity}, {"text", std::move(text)}}; }

}  // namespace

RoutePlan RoutePlan::fromMission(const json& mission, const std::optional<json>& routeResult) {
    RoutePlan plan;
    const json& pilot = mission.at("pilot");
    const auto point = [](const json& p) { return rfcore::GeoPoint{p.at("lat").get<double>(), p.at("lon").get<double>()}; };
    std::vector names{pointLabel(pilot, "Pilot")};
    plan.points.push_back(point(pilot));
    const json& wps = mission.at("waypoints");
    for (std::size_t i = 0; i < wps.size(); ++i) {
        plan.points.push_back(point(wps[i]));
        names.push_back(pointLabel(wps[i], std::format("WP{}", i + 1)));
    }
    if (mission.value("returnToPilot", true)) {
        plan.points.push_back(point(pilot));
        names.push_back(names.front());
    }
    const rfcore::Geodesy geo;
    for (std::size_t i = 1; i < plan.points.size(); ++i) {
        plan.legs.push_back({names[i - 1], names[i], geo.inverse(plan.points[i - 1], plan.points[i]).distance.value(), {}, {}});
    }
    if (routeResult && routeResult->is_object()) plan.readResult(*routeResult);
    return plan;
}

void RoutePlan::readResult(const json& result) {
    const json legsJson = result.value("legs", json::array());
    for (std::size_t i = 0; i < legsJson.size() && i < legs.size(); ++i) {
        legs[i].controlMarginDb = marginOf(legsJson[i], "control");
        legs[i].videoMarginDb = marginOf(legsJson[i], "video");
    }
    const json modules = result.value("modules", json::object());
    if (modules.contains("fpv.battery")) neededWh = number(modules.at("fpv.battery"), "neededWh");
}

double RoutePlan::lengthM() const {
    double total = 0.0;
    for (const PlannedLeg& l : legs) total += l.lengthM;
    return total;
}

RouteGeometry::RouteGeometry(const std::vector<rfcore::GeoPoint>& points)
    : frame_(points.empty() ? 0.0 : points.front().latitudeDeg, points.empty() ? 0.0 : points.front().longitudeDeg, 0.0) {
    for (const rfcore::GeoPoint& p : points) points_.push_back(project(p.latitudeDeg, p.longitudeDeg));
}

RouteGeometry::Xy RouteGeometry::project(double lat, double lon) const {
    Xy out;
    double z = 0.0;
    frame_.Forward(lat, lon, 0.0, out.x, out.y, z);
    return out;
}

std::optional<RouteGeometry::Nearest> RouteGeometry::nearest(double lat, double lon) const {
    if (points_.size() < 2) return std::nullopt;
    const Xy p = project(lat, lon);
    std::optional<Nearest> best;
    for (std::size_t i = 1; i < points_.size(); ++i) {
        const Xy& a = points_[i - 1];
        const Xy& b = points_[i];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double len2 = dx * dx + dy * dy;
        const double f = len2 > 0.0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(p.x - (a.x + f * dx), p.y - (a.y + f * dy));
        if (!best || d < best->distanceM) best = Nearest{i - 1, d};
    }
    return best;
}

PlanComparison::Track PlanComparison::follow(const RoutePlan& plan, const Timeline& timeline) const {
    Track track;
    track.legs.resize(plan.legs.size());
    track.offRouteM.assign(timeline.size(), kNoValue);
    const RouteGeometry route(plan.points);
    const std::string link = SummaryBuilder::linkChannel(timeline);
    const double step = timeline.hz > 0.0 ? 1.0 / timeline.hz : 0.0;
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        const double lat = timeline.at("lat", i);
        const double lon = timeline.at("lon", i);
        if (const double armed = timeline.at("armed", i); !hasValue(lat) || !hasValue(lon) || (hasValue(armed) && armed < 0.5)) {
            continue;
        }
        const auto near = route.nearest(lat, lon);
        if (!near) continue;
        track.offRouteM[i] = near->distanceM;
        LegFact& leg = track.legs[near->leg];
        leg.seconds += step;
        leg.maxOffRouteM = std::max(leg.maxOffRouteM, near->distanceM);
        if (const double q = timeline.at(link, i); hasValue(q)) leg.minLinkPct = std::min(q, leg.minLinkPct.value_or(q));
        if (!track.worst.has_value() || near->distanceM > track.offRouteM[*track.worst]) {
            track.worst = i;
            track.worstLeg = near->leg;
        }
    }
    return track;
}

json PlanComparison::rows(const RoutePlan& plan, const FlightSummary& summary, const Track& track) const {
    std::optional<double> plannedMargin;
    for (const PlannedLeg& l : plan.legs) {
        if (l.controlMarginDb.has_value()) plannedMargin = std::min(*l.controlMarginDb, plannedMargin.value_or(*l.controlMarginDb));
    }
    std::optional<double> planWhKm;
    std::optional<double> factWhKm;
    if (plan.neededWh.has_value() && plan.lengthM() > 0.0) planWhKm = *plan.neededWh / (plan.lengthM() / 1000.0);
    if (summary.usedWh.has_value() && summary.distanceM.has_value() && *summary.distanceM > 0.0) factWhKm = *summary.usedWh / (*summary.distanceM / 1000.0);
    const std::optional<double> offRoute = track.worst.has_value() ? std::optional(track.offRouteM[*track.worst]) : std::nullopt;
    json out = json::array();
    out.push_back(row("distance", "Distance", "m", plan.lengthM(), summary.distanceM));
    out.push_back(row("energy", "Energy", "Wh", plan.neededWh, summary.usedWh));
    out.push_back(row("whPerKm", "Energy per km", "Wh/km", planWhKm, factWhKm));
    out.push_back(row("weakestLink", "Weakest control link", summary.linkChannel == "linkQualityPct" ? "dB / LQ %" : "dB / RSSI %",
                      plannedMargin, summary.minLinkPct));
    out.back().erase("deltaPct");  // a margin in dB and a percentage do not compare as a ratio
    out.push_back(row("maxOffRoute", "Furthest off the route", "m", 0.0, offRoute));
    out.back().erase("deltaPct");
    return out;
}

json PlanComparison::legsJson(const RoutePlan& plan, const Track& track) {
    json out = json::array();
    for (std::size_t i = 0; i < plan.legs.size(); ++i) {
        const PlannedLeg& p = plan.legs[i];
        const LegFact& f = track.legs[i];
        out.push_back({{"index", i},
                       {"from", p.from},
                       {"to", p.to},
                       {"lengthM", std::round(p.lengthM * 10.0) / 10.0},
                       {"controlMarginDb", optionalJson(p.controlMarginDb)},
                       {"videoMarginDb", optionalJson(p.videoMarginDb)},
                       {"flownS", std::round(f.seconds * 10.0) / 10.0},
                       {"minLinkPct", optionalJson(f.minLinkPct)},
                       {"maxOffRouteM", std::round(f.maxOffRouteM * 10.0) / 10.0}});
    }
    return out;
}

json PlanComparison::findings(const RoutePlan& plan, const FlightSummary& summary, const Track& track,
                              const Timeline& timeline) const {
    json out = json::array();
    if (summary.distanceM.has_value() && plan.lengthM() > 0.0) out.push_back(distanceFinding(plan, *summary.distanceM));
    if (plan.neededWh.has_value() && (!summary.usedWh.has_value() || *plan.neededWh > 0.0)) out.push_back(energyFinding(*plan.neededWh, summary.usedWh));
    out.push_back(routeFinding(plan, track, timeline));
    for (std::size_t i = 0; i < plan.legs.size(); ++i) {
        if (auto f = legFinding(plan.legs[i], track.legs[i])) out.push_back(std::move(*f));
    }
    return out;
}

json PlanComparison::distanceFinding(const RoutePlan& plan, double flownM) {
    const double pct = (flownM - plan.lengthM()) / plan.lengthM() * 100.0;
    return finding(std::abs(pct) > 25.0 ? "warn" : "info", "Flew " + km(flownM) + " against " + km(plan.lengthM()) +
                                                               " planned (" + (pct >= 0 ? "+" : "") + fixed(pct, 0) + " %).");
}

json PlanComparison::energyFinding(double neededWh, std::optional<double> usedWh) {
    if (!usedWh.has_value()) return finding("info", "The log has no current sensor, so energy cannot be compared with the plan.");
    const double pct = *usedWh / neededWh * 100.0;
    return finding(pct > 110.0 ? "warn" : "info", "Used " + fixed(*usedWh, 1) + " Wh against " + fixed(neededWh, 1) +
                                                      " Wh planned (" + fixed(pct, 0) + " %).");
}

json PlanComparison::routeFinding(const RoutePlan& plan, const Track& track, const Timeline& timeline) const {
    if (!track.worst.has_value()) return finding("info", "The log has no GPS track while armed, so the route cannot be compared.");
    const double d = track.offRouteM[*track.worst];
    const PlannedLeg& leg = plan.legs[track.worstLeg];
    return finding(d > options_.offRouteWarnM ? "warn" : "info",
                   "Furthest off the route: " + fixed(d, 0) + " m, on the leg " + leg.from + " → " + leg.to + " at " +
                       fixed(timeline.samples.t[*track.worst], 0) + " s.");
}

std::optional<json> PlanComparison::legFinding(const PlannedLeg& p, const LegFact& f) {
    if (!p.controlMarginDb.has_value() || !f.minLinkPct.has_value() || *p.controlMarginDb < 6.0 || *f.minLinkPct >= 30.0) return std::nullopt;
    return finding("warn", "Leg " + p.from + " → " + p.to + ": planned " + fixed(*p.controlMarginDb, 0) +
                               " dB of control margin, but the link fell to " + fixed(*f.minLinkPct, 0) +
                               " %. Check antennas, obstacles and the radio settings.");
}

json PlanComparison::compare(const RoutePlan& plan, const Timeline& timeline, const FlightSummary& summary) const {
    const Track track = follow(plan, timeline);
    json offRoute = json::array();
    for (const double v : track.offRouteM) offRoute.push_back(hasValue(v) ? json(std::round(v * 10.0) / 10.0) : json(nullptr));
    json out = {{"rows", rows(plan, summary, track)},
                {"legs", legsJson(plan, track)},
                {"findings", findings(plan, summary, track, timeline)},
                {"offRouteM", offRoute}};
    if (track.worst.has_value()) {
        const std::size_t i = *track.worst;
        out["furthest"] = {{"t", timeline.samples.t[i]}, {"lat", timeline.at("lat", i)}, {"lon", timeline.at("lon", i)},
                           {"distanceM", track.offRouteM[i]}, {"leg", track.worstLeg}};
    }
    return out;
}

}  // namespace rfflightlog
