#include "host_services.hpp"

#include "logger.hpp"
#include "module_runner.hpp"
#include "schemas.hpp"

#include <rfcore/geodesy.hpp>

#include <rfmod/abi.h>

#include <format>

namespace rfmap {

namespace {
thread_local const CalcContext* currentContext = nullptr;
thread_local int hostDepth = 0;

struct DepthGuard {
    DepthGuard() { ++hostDepth; }
    ~DepthGuard() { --hostDepth; }
};

rfcore::GeoPoint geo(const json& p) { return {p.at("lat").get<double>(), p.at("lon").get<double>()}; }

json geodesyInverse(const json& request) {
    const auto r = rfcore::Geodesy().inverse(geo(request.at("from")), geo(request.at("to")));
    return {{"distanceM", r.distance.value()}, {"azimuth1Deg", r.azimuthStartDeg}, {"azimuth2Deg", r.azimuthEndDeg}};
}

json geodesyDirect(const json& request) {
    const auto r = rfcore::Geodesy().directWithAzimuth(geo(request.at("from")), request.at("azimuthDeg").get<double>(),
                                                       rfcore::Metres(request.at("distanceM").get<double>()));
    return {{"lat", r.point.latitudeDeg}, {"lon", r.point.longitudeDeg}, {"azimuthDeg", r.azimuthEndDeg}};
}
}  // namespace

CalcContextScope::CalcContextScope(const CalcContext& ctx) : previous_(currentContext) { currentContext = &ctx; }
CalcContextScope::~CalcContextScope() { currentContext = previous_; }
const CalcContext* CalcContextScope::current() { return currentContext; }

ServerHostServices::ServerHostServices(LinkService* links, const Library* library, WeatherService* weather,
                                       ContextFactory contexts, Logger& log)
    : links_(links), library_(library), weather_(weather), contexts_(std::move(contexts)), log_(log) {}

void ServerHostServices::bind(LinkService* links, const Library* library, WeatherService* weather,
                              ContextFactory contexts, const SchemaRegistry* schemas) {
    schemas_ = schemas;
    links_ = links;
    library_ = library;
    weather_ = weather;
    contexts_ = std::move(contexts);
}

void ServerHostServices::log(const std::string& moduleId, int level, const std::string& message) {
    const Logger::Level l = level >= RFMOD_LOG_ERROR  ? Logger::Level::Error
                            : level >= RFMOD_LOG_WARN ? Logger::Level::Warn
                                                      : Logger::Level::Info;
    log_.write(l, "module " + moduleId, message);
}

json ServerHostServices::call(const std::string& moduleId, const std::string& method, const json& request) {
    if (hostDepth >= kMaxDepth) throw HostCallError(std::format("host calls nested deeper than {}", kMaxDepth));
    const DepthGuard depth;
    if (method == "link.evaluate") {
        // The guide's promise: evaluating a link runs loss.contribute, so calling it from there recurses.
        if (!LossContributionScope::active().empty()) {
            throw HostCallError("link.evaluate is refused inside loss.contribute (called by " +
                                     LossContributionScope::active() + "): it would recurse");
        }
        if (!links_) throw HostCallError("link evaluation is not available");
        return linkEvaluate(request);
    }
    if (method == "geodesy.inverse") return geodesyInverse(request);
    if (method == "geodesy.direct") return geodesyDirect(request);
    if (method == "elevation.point") return elevationPoint(request);
    if (method == "profile.get") return profileGet(request);
    if (method == "weather.point") return weatherPoint(request);
    if (method == "library.get") return libraryGet(request);
    (void)moduleId;
    throw HostCallError("unknown host method " + method);
}

json ServerHostServices::elevationPoint(const json& req) const {
    if (!links_) throw HostCallError("no terrain model");
    const DemSource& dem = links_->requireDem();
    const auto e = dem.elevation(geo(req));
    const std::optional<double> statedError = dem.statedErrorM();
    return {{"groundMslM", e.has_value() ? json(e->heightMsl.value()) : json(nullptr)},
            {"uncertaintyM", statedError.has_value() ? json(*statedError) : json(nullptr)},
            {"source", dem.name()}};
}

json ServerHostServices::weatherPoint(const json& req) {
    if (!weather_) throw HostCallError("no weather service");
    const auto t = req.contains("time") ? parseIsoUtc(req.at("time").get<std::string>()) : Clock::now();
    if (!t.has_value()) throw HostCallError("time must be ISO 8601");
    return weather_->at(req.at("lat").get<double>(), req.at("lon").get<double>(), *t).point;
}

json ServerHostServices::libraryGet(const json& req) const {
    if (!library_) throw HostCallError("no library");
    const auto item = library_->get(req.at("kind").get<std::string>(), req.at("id").get<std::string>());
    if (!item.has_value()) throw HostCallError("no library item " + req.value("kind", "") + "/" + req.value("id", ""));
    return *item;
}

json ServerHostServices::profileGet(const json& req) {
    if (!links_) throw HostCallError("no terrain model");
    const DemSource& dem = links_->requireDem();
    const rfcore::Geodesy g;
    const auto from = geo(req.at("from"));
    const auto to = geo(req.at("to"));
    const double total = g.inverse(from, to).distance.value();
    double step = dem.nativeResolution().value();
    if (req.contains("stepM")) step = std::min(step, req.at("stepM").get<double>());
    if (!(step > 0.0)) throw HostCallError("stepM must be positive");
    const auto points = g.sample(from, to, rfcore::Metres(step));
    const double n = static_cast<double>(points.size() - 1);
    json samples = json::array();
    json missing = json::array();
    std::optional<double> gap;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double d = total * static_cast<double>(i) / n;
        const auto e = dem.elevation(points[i]);
        json s = {{"distanceM", d}, {"lat", points[i].latitudeDeg}, {"lon", points[i].longitudeDeg}};
        if (e) {
            s["groundMslM"] = e->heightMsl.value();
            s["uncertaintyM"] = e->uncertainty.value();
            if (gap.has_value()) missing.push_back({{"fromM", *gap}, {"toM", d}});
            gap.reset();
        } else {
            s["groundMslM"] = nullptr;
            if (!gap.has_value()) gap = d;
        }
        s["obstacleM"] = 0.0;
        samples.push_back(std::move(s));
    }
    if (gap.has_value()) missing.push_back({{"fromM", *gap}, {"toM", total}});
    return {{"source", dem.name()}, {"stepM", total / n}, {"samples", samples}, {"missing", missing}};
}

json ServerHostServices::linkEvaluate(const json& req) {
    auto missingError = [](const PreparedPath& p) {
        std::string spans;
        for (const auto& s : p.missing) {
            spans += (spans.empty() ? "" : ", ") + std::to_string(s.from.value()) + "-" + std::to_string(s.to.value()) + " m";
        }
        return HostCallError("missing-data: no elevation data on " + spans + " of the path; nothing is guessed");
    };
    if (req.contains("mission")) {
        const json& mission = req.at("mission");
        // The mission need not be stored (fpv.core sends patched copies), so check it here.
        if (schemas_) {
            const auto problems = schemas_->check("mission", mission);
            if (!problems.empty()) {
                throw HostCallError("bad_request: mission does not match mission.schema.json: " +
                                         problems.front().pointer + " " + problems.front().message);
            }
        }
        const LinkKind kind = linkKindFromString(req.at("link").get<std::string>());
        const json& links = mission.at("links");
        if (!links.contains(toString(kind))) throw HostCallError("the mission has no " + toString(kind) + " link");
        const CalcContext ctx = contexts_(mission);
        const CalcContextScope scope(ctx);
        const ResolvedLink link = links_->resolve(links.at(toString(kind)), kind);
        const Terminal ground = Terminal::fromJson(req.contains("ground") ? req.at("ground") : mission.at("pilot"));
        const Terminal drone = Terminal::fromJson(req.at("drone"));
        const PreparedPath path = links_->prepare(ground, drone, ctx);
        if (!path.complete()) throw missingError(path);
        return links_->evaluate(path, link, ctx, true, false).result;
    }
    CalcContext ctx;
    if (const CalcContext* cur = CalcContextScope::current()) ctx = *cur;
    else ctx.time = Clock::now();
    if (req.contains("time")) {
        const auto t = parseIsoUtc(req.at("time").get<std::string>());
        if (!t) throw HostCallError("time must be ISO 8601");
        ctx.time = *t;
    }
    const json& tx = req.at("tx");
    const json& rx = req.at("rx");
    LinkService::RawEnds e{req.at("frequencyHz").get<double>(),
                           req.at("txPowerDbm").get<double>(),
                           Terminal::fromJson(tx),
                           Terminal::fromJson(rx),
                           tx.value("gainDbi", 0.0),
                           tx.value("feederLossDb", 0.0),
                           rx.value("gainDbi", 0.0),
                           rx.value("feederLossDb", 0.0),
                           rx.at("sensitivityDbm").get<double>(),
                           rx.value("requiredMarginDb", 0.0)};
    try {
        return links_->evaluateRaw(e, ctx).result;
    } catch (const HttpError& err) {
        throw HostCallError(err.body().contains("status") ? "missing-data: " + std::string(err.what()) : err.what());
    }
}

}  // namespace rfmap
