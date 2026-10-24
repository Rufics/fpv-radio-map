#include "calculator.hpp"

#include <format>

namespace rfmap {

Calculator::Calculator(LinkService& links, WeatherService& weather, const BuiltinModules& builtins,
                       const ModuleRunner& modules, ResultCache& cache, const SchemaRegistry& schemas)
    : links_(links),
      weather_(weather),
      builtins_(builtins),
      modules_(modules),
      cache_(cache),
      keys_(links, builtins, modules),
      routes_(links, modules),
      coverage_(links, builtins, modules),
      environment_{links, links.library(), weather, schemas, *this} {}

std::vector<LinkKind> Calculator::kindsFor(const json& mission) {
    const std::string e = mission.value("evaluate", "both");
    std::vector<LinkKind> kinds;
    if (e == "both" || e == "control") kinds.push_back(LinkKind::Control);
    if (e == "both" || e == "video") kinds.push_back(LinkKind::Video);
    const json links = mission.value("links", json::object());
    for (LinkKind k : kinds) {
        if (!links.contains(toString(k))) {
            throw HttpError(422, "evaluate asks for the " + toString(k) + " link but links." + toString(k) + " is not set");
        }
    }
    return kinds;
}

CalcContext Calculator::withModules(CalcContext ctx, const json& missionModules, const json& pilot) const {
    ctx.missionModules = missionModules;
    // The schema admits any object under modules.<id>; the module's own options schema decides.
    if (const auto problems = modules_.checkOptions(ctx.missionModules); !problems.empty()) {
        json issues = json::array();
        for (const auto& p : problems) issues.push_back({{"path", "/modules"}, {"message", p}});
        throw HttpError(422, "module options do not match the module's schema: " + problems.front(), {{"issues", issues}});
    }
    if (builtins_.enabled(BuiltinModules::kWeather) && ctx.optedIn(BuiltinModules::kWeather)) {
        ctx.forecast = weather_.at(pilot.at("lat").get<double>(), pilot.at("lon").get<double>(), ctx.time);
    }
    return ctx;
}

CalcContext Calculator::contextFor(const json& mission) const {
    CalcContext ctx;
    if (const json& t = mission.at("time"); t.value("mode", "now") == "planned") {
        if (!t.contains("utc")) throw HttpError(422, "time.mode is planned but time.utc is missing");
        const auto parsed = UtcTime::parse(t.at("utc").get<std::string>());
        if (!parsed) throw HttpError(422, "time.utc is not an ISO 8601 date-time");
        ctx.time = *parsed;
    } else {
        ctx.time = Clock::now();
    }
    return withModules(std::move(ctx), mission.value("modules", json::object()), mission.at("pilot"));
}

RouteEvaluator::Links Calculator::resolveLinks(const json& links, const std::vector<LinkKind>& kinds) const {
    RouteEvaluator::Links out;
    for (LinkKind k : kinds) out.emplace_back(k, links_.resolve(links.at(toString(k)), k));
    return out;
}

json Calculator::path(const json& req) {
    CalcContext ctx;
    ctx.time = Clock::now();
    if (req.contains("timeUtc")) {
        const auto t = UtcTime::parse(req.at("timeUtc").get<std::string>());
        if (!t) throw HttpError(422, "timeUtc is not an ISO 8601 date-time");
        ctx.time = *t;
    }
    ctx = withModules(std::move(ctx), req.value("modules", json::object()), req.at("from"));
    const Terminal ground = Terminal::fromJson(req.at("from"));
    const Terminal air = Terminal::fromJson(req.at("to"));

    RouteEvaluator::Links links;
    const bool both = req.contains("links");
    if (both) {
        const json m = {{"evaluate", req.value("evaluate", "both")}, {"links", req.at("links")}};
        links = resolveLinks(req.at("links"), kindsFor(m));
        if (links.empty()) throw HttpError(422, "evaluate none asks for no link; a path calculation needs one");
    } else {
        const LinkKind k = linkKindFromString(req.at("kind").get<std::string>());
        links.emplace_back(k, links_.resolve(req.at("link"), k));
    }
    LibraryRefList refs;
    for (const auto& l : links | std::views::values) refs.addAll(l.refs);
    const std::string hash = keys_.hash({{"kind", "path"}, {"request", req}}, refs, ctx);
    if (auto hit = cache_.get(hash)) return *hit;

    const InvocationScope scope(environment_, ctx);
    const PreparedPath p = links_.prepare(ground, air, ctx);
    if (!p.complete()) {
        throw HttpError(422, "missing elevation data on part of the path; nothing is guessed",
                        links_.missingData(p.missing, p.ground.position, p.air.position));
    }
    json out = json::object();
    for (const auto& [k, l] : links) {
        LinkEvaluation e = links_.evaluate(p, l, ctx, true, true);
        e.result["inputHash"] = hash;
        if (ctx.forecast) e.result["weather"] = ctx.forecast->point;
        e.result["forecastTier"] = ctx.forecastTier();
        out[toString(k)] = std::move(e.result);
    }
    json result = both ? out : out.begin().value();
    cache_.put(hash, "path", result);
    return result;
}

json Calculator::route(const json& mission, std::optional<int> revision) {
    const CalcContext ctx = contextFor(mission);
    const RouteEvaluator::Links links = resolveLinks(mission.value("links", json::object()), kindsFor(mission));
    LibraryRefList refs;
    for (const auto& l : links | std::views::values) refs.addAll(l.refs);
    for (const char* kind : {"drones", "batteries"}) {
        const char* field = std::string(kind) == "drones" ? "drone" : "battery";
        if (!mission.contains(field)) continue;
        if (const auto item = links_.library().get(kind, mission.at(field).get<std::string>())) {
            refs.add(LibraryRef{kind, item->at("id"), item->at("version"), item->at("verified")}.toJson());
        }
    }
    const std::string hash = keys_.hash({{"kind", "route"}, {"mission", mission}}, refs, ctx);
    if (auto hit = cache_.get(hash)) {
        if (revision.has_value()) (*hit)["missionRevision"] = *revision;
        return *hit;
    }

    const InvocationScope scope(environment_, ctx);
    json result = {{"inputHash", hash}, {"legs", json::array()}, {"modules", json::object()},
                   {"moduleFailures", json::array()}, {"reasons", json::array()}, {"library", refs.toJson()},
                   {"timeUtc", UtcTime::format(ctx.time)}};
    if (revision.has_value()) result["missionRevision"] = *revision;
    if (ctx.forecast) result["weather"] = ctx.forecast->point;
    result["forecastTier"] = ctx.forecastTier();
    routes_.evaluate(mission, links, ctx, result);
    // Only a complete, failure-free result is worth keeping: a module that failed might work next time.
    if (result["moduleFailures"].empty()) cache_.put(hash, "route", result);
    return result;
}

json Calculator::coverage(const json& mission, LinkKind kind, CoverageProgress& progress, ThreadPool& pool) {
    const CalcContext ctx = contextFor(mission);
    if (!mission.value("links", json::object()).contains(toString(kind))) {
        throw HttpError(422, "the mission has no " + toString(kind) + " link");
    }
    const ResolvedLink link = links_.resolve(mission.at("links").at(toString(kind)), kind);
    LibraryRefList refs;
    for (const auto& r : link.refs) refs.add(r.toJson());
    const std::string hash = keys_.hash({{"kind", "coverage"}, {"link", toString(kind)}, {"mission", mission}}, refs, ctx);
    if (auto hit = cache_.get(hash)) {
        (*hit)["cached"] = true;
        return *hit;
    }
    const InvocationScope scope(environment_, ctx);
    json grid = coverage_.run({mission, kind, link, ctx, refs.toJson(), hash}, progress, pool);
    if (grid["moduleFailures"].empty()) cache_.put(hash, "coverage", grid);
    return grid;
}

}  // namespace rfmap
