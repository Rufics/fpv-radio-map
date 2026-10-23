#include "host_methods.hpp"

#include "library.hpp"
#include "schemas.hpp"
#include "weather.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace rfmap {

// ---- HostRequest ----

HostRequest::HostRequest(const json& request) : request_(request) {
    if (!request_.is_object()) throw HostCallError("bad_request: the request must be an object");
}

bool HostRequest::has(std::string_view field) const { return request_.contains(field); }

const json& HostRequest::field(std::string_view name) const {
    const auto it = request_.find(name);
    if (it == request_.end()) throw HostCallError(std::format("bad_request: {} is required", name));
    return *it;
}

double HostRequest::number(std::string_view name, double min, double max) const {
    const json& v = field(name);
    if (!v.is_number()) throw HostCallError(std::format("bad_request: {} must be a number", name));
    const double d = v.get<double>();
    if (!std::isfinite(d) || d < min || d > max) {
        throw HostCallError(std::format("bad_request: {} must be between {} and {}", name, min, max));
    }
    return d;
}

std::optional<double> HostRequest::optionalNumber(std::string_view name, double min, double max) const {
    if (!has(name)) return std::nullopt;
    return number(name, min, max);
}

std::string HostRequest::string(std::string_view name, std::size_t maxLength) const {
    const json& v = field(name);
    if (!v.is_string()) throw HostCallError(std::format("bad_request: {} must be a string", name));
    const auto s = v.get<std::string>();
    if (s.empty() || s.size() > maxLength) {
        throw HostCallError(std::format("bad_request: {} must be 1 to {} characters", name, maxLength));
    }
    return s;
}

rfcore::GeoPoint HostRequest::point(std::string_view name) const {
    const HostRequest p(object(name));
    return {p.number("lat", -90.0, 90.0), p.number("lon", -180.0, 180.0)};
}

const json& HostRequest::object(std::string_view name) const {
    const json& v = field(name);
    if (!v.is_object()) throw HostCallError(std::format("bad_request: {} must be an object", name));
    return v;
}

TimePoint HostRequest::time(std::string_view name, TimePoint fallback) const {
    if (!has(name)) return fallback;
    const auto t = UtcTime::parse(string(name, 64));
    if (!t) throw HostCallError(std::format("bad_request: {} must be ISO 8601", name));
    return *t;
}

// ---- the methods ----

namespace {

json pointJson(const rfcore::GeoPoint& p) { return {{"lat", p.latitudeDeg}, {"lon", p.longitudeDeg}}; }

class GeodesyInverse final : public IHostMethod {
public:
    [[nodiscard]] std::string_view name() const override { return "geodesy.inverse"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope&) const override {
        const auto r = rfcore::Geodesy().inverse(req.point("from"), req.point("to"));
        return {{"distanceM", r.distance.value()}, {"azimuth1Deg", r.azimuthStartDeg}, {"azimuth2Deg", r.azimuthEndDeg}};
    }
};

class GeodesyDirect final : public IHostMethod {
public:
    [[nodiscard]] std::string_view name() const override { return "geodesy.direct"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope&) const override {
        const auto r = rfcore::Geodesy().directWithAzimuth(
            req.point("from"), req.number("azimuthDeg", -360.0, 360.0),
            rfcore::Metres(req.number("distanceM", -HostRequest::kMaxDistanceM, HostRequest::kMaxDistanceM)));
        json out = pointJson(r.point);
        out["azimuthDeg"] = r.azimuthEndDeg;
        return out;
    }
};

class ElevationPoint final : public IHostMethod {
public:
    [[nodiscard]] std::string_view name() const override { return "elevation.point"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope& scope) const override {
        const DemSource& dem = scope.environment().links.requireDem();
        const auto e = dem.elevation({req.number("lat", -90.0, 90.0), req.number("lon", -180.0, 180.0)});
        const std::optional<double> statedError = dem.statedErrorM();
        return {{"groundMslM", e.has_value() ? json(e->heightMsl.value()) : json(nullptr)},
                {"uncertaintyM", statedError.has_value() ? json(*statedError) : json(nullptr)},
                {"source", dem.name()}};
    }
};

/// The terrain profile between two points. The sample count is bounded: the step is at least a
/// metre and the path at most kMaxSamples steps, whatever the request asks.
class ProfileGet final : public IHostMethod {
public:
    static constexpr double kMinStepM = 1.0;
    static constexpr double kMaxSamples = 20000.0;

    [[nodiscard]] std::string_view name() const override { return "profile.get"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope& scope) const override {
        const DemSource& dem = scope.environment().links.requireDem();
        const rfcore::Geodesy g;
        const auto from = req.point("from");
        const auto to = req.point("to");
        const double total = g.inverse(from, to).distance.value();
        double step = std::max(kMinStepM, dem.nativeResolution().value());
        // The module's step replaces the DEM's: coarser for a long path, finer where it wants detail.
        if (const auto asked = req.optionalNumber("stepM", kMinStepM, HostRequest::kMaxDistanceM); asked.has_value()) {
            step = *asked;
        }
        if (total / step > kMaxSamples) {
            throw HostCallError(std::format("bad_request: the path needs more than {} samples at {} m; use a larger stepM",
                                            static_cast<long>(kMaxSamples), step));
        }
        const auto points = g.sample(from, to, rfcore::Metres(step));
        return profile(dem, points, total);
    }

private:
    [[nodiscard]] static json profile(const DemSource& dem, const std::vector<rfcore::GeoPoint>& points, double total) {
        const auto n = static_cast<double>(points.size() - 1);
        json samples = json::array();
        json missing = json::array();
        std::optional<double> gap;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double d = total * static_cast<double>(i) / n;
            const auto e = dem.elevation(points[i]);
            json s = pointJson(points[i]);
            s["distanceM"] = d;
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
};

class WeatherPoint final : public IHostMethod {
public:
    [[nodiscard]] std::string_view name() const override { return "weather.point"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope& scope) const override {
        const double lat = req.number("lat", -90.0, 90.0);
        const double lon = req.number("lon", -180.0, 180.0);
        // The request's time, not the wall clock: the same hour the calculation itself uses.
        return scope.environment().weather.at(lat, lon, req.time("time", scope.context().time)).point;
    }
};

class LibraryGet final : public IHostMethod {
public:
    [[nodiscard]] std::string_view name() const override { return "library.get"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope& scope) const override {
        const std::string kind = req.string("kind", 64);
        const std::string id = req.string("id", 128);
        const auto item = scope.environment().library.get(kind, id);
        if (!item.has_value()) throw HostCallError("no library item " + kind + "/" + id);
        return *item;
    }
};

/// link.evaluate: a mission's link at two ends, or raw radio parameters.
class LinkEvaluate final : public IHostMethod {
public:
    static constexpr double kMaxAglM = 100'000.0;  ///< above any aircraft the model is for

    [[nodiscard]] std::string_view name() const override { return "link.evaluate"; }
    [[nodiscard]] json call(const HostRequest& req, const InvocationScope& scope) const override {
        // The guide's promise: evaluating a link runs loss.contribute, so calling it from there recurses.
        if (!scope.contributor().empty()) {
            throw HostCallError("link.evaluate is refused inside loss.contribute (called by " + scope.contributor() +
                                "): it would recurse");
        }
        return req.has("mission") ? withMission(req, scope) : raw(req, scope);
    }

private:
    [[nodiscard]] static HostCallError missingError(const PreparedPath& p) {
        std::string spans;
        for (const auto& s : p.missing) {
            spans += std::format("{}{:f}-{:f} m", spans.empty() ? "" : ", ", s.from.value(), s.to.value());
        }
        return HostCallError("missing-data: no elevation data on " + spans + " of the path; nothing is guessed");
    }

    /// A path end, every field checked for shape and range like any other host request field.
    [[nodiscard]] static Terminal terminal(const HostRequest& end) {
        return {{end.number("lat", -90.0, 90.0), end.number("lon", -180.0, 180.0)}, end.number("aglM", 0.0, kMaxAglM)};
    }

    [[nodiscard]] static json withMission(const HostRequest& req, const InvocationScope& scope) {
        const HostEnvironment& env = scope.environment();
        const json& mission = req.object("mission");
        // The mission need not be stored (fpv.core sends patched copies), so check it here.
        if (const auto problems = env.schemas.check("mission", mission); !problems.empty()) {
            throw HostCallError("bad_request: mission does not match mission.schema.json: " + problems.front().pointer +
                                " " + problems.front().message);
        }
        const LinkKind kind = linkKindFromString(req.string("link", 16));
        const json& links = mission.at("links");
        if (!links.contains(toString(kind))) throw HostCallError("the mission has no " + toString(kind) + " link");
        const CalcContext ctx = env.contexts.contextFor(mission);
        const InvocationScope nested(ctx);
        const ResolvedLink link = env.links.resolve(links.at(toString(kind)), kind);
        const Terminal ground = terminal(HostRequest(req.has("ground") ? req.object("ground") : mission.at("pilot")));
        const Terminal drone = terminal(HostRequest(req.object("drone")));
        const PreparedPath path = env.links.prepare(ground, drone, ctx);
        if (!path.complete()) throw missingError(path);
        return env.links.evaluate(path, link, ctx, true, false).result;
    }

    [[nodiscard]] static json raw(const HostRequest& req, const InvocationScope& scope) {
        CalcContext ctx = scope.context();
        ctx.time = req.time("time", ctx.time);
        const HostRequest tx(req.object("tx"));
        const HostRequest rx(req.object("rx"));
        LinkService::RawEnds e{req.number("frequencyHz", 1e3, 1e12),
                               req.number("txPowerDbm", -100.0, 100.0),
                               terminal(tx),
                               terminal(rx),
                               tx.optionalNumber("gainDbi", -50.0, 100.0).value_or(0.0),
                               tx.optionalNumber("feederLossDb", 0.0, 100.0).value_or(0.0),
                               rx.optionalNumber("gainDbi", -50.0, 100.0).value_or(0.0),
                               rx.optionalNumber("feederLossDb", 0.0, 100.0).value_or(0.0),
                               rx.number("sensitivityDbm", -200.0, 50.0),
                               rx.optionalNumber("requiredMarginDb", -100.0, 100.0).value_or(0.0)};
        try {
            return scope.environment().links.evaluateRaw(e, ctx).result;
        } catch (const HttpError& err) {
            throw HostCallError(err.body().contains("status") ? "missing-data: " + std::string(err.what()) : err.what());
        }
    }
};

}  // namespace

HostMethodTable::HostMethodTable() {
    methods_.push_back(std::make_unique<LinkEvaluate>());
    methods_.push_back(std::make_unique<GeodesyInverse>());
    methods_.push_back(std::make_unique<GeodesyDirect>());
    methods_.push_back(std::make_unique<ElevationPoint>());
    methods_.push_back(std::make_unique<ProfileGet>());
    methods_.push_back(std::make_unique<WeatherPoint>());
    methods_.push_back(std::make_unique<LibraryGet>());
}

const IHostMethod* HostMethodTable::find(std::string_view name) const {
    const auto it = std::ranges::find_if(methods_, [name](const auto& m) { return m->name() == name; });
    return it == methods_.end() ? nullptr : it->get();
}

std::vector<std::string_view> HostMethodTable::names() const {
    std::vector<std::string_view> out;
    out.reserve(methods_.size());
    for (const auto& m : methods_) out.push_back(m->name());
    return out;
}

}  // namespace rfmap
