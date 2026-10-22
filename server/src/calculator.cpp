#include "calculator.hpp"

#include "host_services.hpp"

#include <rfcore/constants.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/geodesy.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <future>

namespace rfmap {

namespace {

// Both links often share antennas; each item version is listed once.
void addRef(nlohmann::json& refs, const nlohmann::json& ref) {
    for (const auto& r : refs) {
        if (r.at("kind") == ref.at("kind") && r.at("id") == ref.at("id") && r.at("version") == ref.at("version")) return;
    }
    refs.push_back(ref);
}

// Which forecast tier fed the calculation: hourly | long-range | none (beyond the forecast,
// k = 4/3) | off (rf.weather not opted in by the mission, or disabled by the admin).
std::string forecastTier(const CalcContext& ctx) {
    return ctx.forecast ? ctx.forecast->point.value("tier", "none") : "off";
}

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

/// Adds a failing module to `list` unless it is listed already.
void appendModuleFailure(json& list, const ModuleFailure& f) {
    const bool seen = std::ranges::any_of(list, [&f](const json& j) { return j.value("module", "") == f.moduleId; });
    if (!seen) list.push_back(f.toJson());
}

/// Adds each failing module to `list` once, keeping the entries already there.
void appendModuleFailures(json& list, const std::vector<ModuleFailure>& failures) {
    for (const auto& f : failures) appendModuleFailure(list, f);
}

// ---- route legs ----

/// The worst sample of one link along a leg.
struct WorstSample {
    double margin = std::numeric_limits<double>::infinity();
    std::optional<PreparedPath> path;
    double along = 0.0;
};

/// What sampling a leg found: counts, the positions without terrain, the worst per link.
struct LegScan {
    std::map<LinkKind, WorstSample> worst;
    std::size_t evaluated = 0;
    std::size_t overPilot = 0;
    std::size_t missingCount = 0;
    json missing = json::array();
    json missingBbox = nullptr;
};

/// Records a drone position whose path from the pilot lacks terrain data; the first 20 are listed.
void recordMissing(LegScan& scan, const LinkService& links, const PreparedPath& p, double along) {
    ++scan.missingCount;
    const json md = links.missingData(p.missing, p.ground.position, p.air.position);
    scan.missingBbox = LinkService::unionBbox(scan.missingBbox, md.at("missingData").at("bbox"));
    if (scan.missing.size() < 20) {
        scan.missing.push_back({{"alongLegM", along},
                                {"lat", p.air.position.latitudeDeg},
                                {"lon", p.air.position.longitudeDeg},
                                {"missing", md.at("missing")}});
    }
}

/// Evaluates every link at one sample position and keeps each link's worst.
void recordWorst(LegScan& scan, const LinkService& service, const PreparedPath& p, double along,
                 const std::vector<std::pair<LinkKind, ResolvedLink>>& links, const CalcContext& ctx,
                 std::vector<ModuleFailure>& failures) {
    for (const auto& [k, l] : links) {
        const LinkEvaluation e = service.evaluate(p, l, ctx, false, false);
        failures.insert(failures.end(), e.failures.begin(), e.failures.end());
        WorstSample& w = scan.worst[k];
        if (e.marginDb < w.margin) {
            w.margin = e.marginDb;
            w.path = p;
            w.along = along;
        }
    }
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

/// Marks a leg with terrain gaps as not evaluated.
json missingDataLeg(json leg, const LegScan& scan, const DemSource& dem, std::size_t total) {
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

// ---- route modules ----

/// Runs one call into a module and returns how it failed, or "" when it did not. A module can
/// fail in any of these ways; the failure is reported in the result, not raised.
template <class Call>
std::string moduleCallFailure(const Call& call) {
    try {
        call();
    } catch (const rfmod::ModuleError& e) {
        return std::format("{}: {}", e.code(), e.what());
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): a module failure is listed, never fails the calculation
        return e.what();
    }
    return {};
}

/// Calls a module's route.analyze. A failure, or an answer that is not an object, is listed in
/// result.moduleFailures (once per module, like its loss term failures) and gives nothing.
std::optional<json> analyzeRoute(const ModuleRunner::Active& a, const json& req, json& result) {
    json resp;
    std::string failure = moduleCallFailure([&a, &req, &resp] { resp = a.instance->invoke("route.analyze", req); });
    if (failure.empty() && !resp.is_object()) failure = "route.analyze did not return an object";
    if (failure.empty()) return std::make_optional(std::move(resp));
    appendModuleFailure(result["moduleFailures"], {a.id, failure});
    return std::nullopt;
}

/// A module's reason with "leg": i belongs to that leg; the rest to the route.
void addRouteReason(json& result, json r, const std::string& moduleId) {
    r["module"] = moduleId;
    if (const std::string sev = r.value("severity", "info"); sev != "info" && sev != "warn" && sev != "blocked") r["severity"] = "info";
    if (r.contains("leg") && r["leg"].is_number_integer()) {
        if (const auto i = r["leg"].get<long long>(); i >= 0 && static_cast<std::size_t>(i) < result["legs"].size()) {
            result["legs"][static_cast<std::size_t>(i)]["reasons"].push_back(r);
            return;
        }
    }
    result["reasons"].push_back(r);
}

// ---- coverage ----

/// A lat/lon-regular grid: cellM is exact at the pilot's latitude (meridian and parallel arc
/// per degree there); further north or south a cell's east-west size changes with the
/// parallel, and cellLatDeg/cellLonDeg say exactly what was used.
struct CoverageGrid {
    double radius = 0.0;
    double agl = 0.0;
    double dLat = 0.0;
    double dLon = 0.0;
    double north = 0.0;
    double west = 0.0;
    std::size_t cols = 0;
    std::size_t rows = 0;
    [[nodiscard]] rfcore::GeoPoint centre(std::size_t r, std::size_t c) const {
        return {north - (static_cast<double>(r) + 0.5) * dLat, west + (static_cast<double>(c) + 0.5) * dLon};
    }
};

CoverageGrid coverageGrid(const rfcore::GeoPoint& pilot, double radius, double cell, double agl) {
    CoverageGrid g{};
    g.radius = radius;
    g.agl = agl;
    const double lat0 = pilot.latitudeDeg;
    const double deg = rfcore::constants::kPi / 180.0;
    const double m = rfcore::EarthModel::radiusOfCurvature(lat0, 0.0).value();
    const double nRad = rfcore::EarthModel::radiusOfCurvature(lat0, 90.0).value();
    g.dLat = cell / (m * deg);
    g.dLon = cell / (nRad * std::cos(lat0 * deg) * deg);
    const auto half = static_cast<std::size_t>(std::ceil(radius / cell));
    g.cols = 2 * half + 1;
    g.rows = 2 * half + 1;
    g.north = lat0 + (static_cast<double>(half) + 0.5) * g.dLat;
    g.west = pilot.longitudeDeg - (static_cast<double>(half) + 0.5) * g.dLon;
    return g;
}

/// A module's coverage.analyze answer in the grid: its reasons, and the layers that match the
/// grid cell for cell.
void addCoverageModule(json& grid, const json& resp, const std::string& moduleId, std::size_t cells) {
    for (json r : resp.value("reasons", json::array())) {
        if (!r.is_object()) continue;
        r["module"] = moduleId;
        grid["reasons"].push_back(r);
    }
    json layers = json::array();
    for (const json& l : resp.value("layers", json::array())) {
        if (l.is_object() && l.contains("values") && l["values"].size() == cells) layers.push_back(l);
    }
    grid["modules"][moduleId] = {{"layers", layers}, {"data", resp.value("data", json::object())}};
}

json nullable(const std::optional<double>& v) { return v.has_value() ? json(*v) : json(nullptr); }

// ---- grid outline ----

/// A cell edge on the integer grid: x = column edge (0..cols), y = row edge (0..rows, down).
struct Edge {
    long x0 = 0;
    long y0 = 0;
    long x1 = 0;
    long y1 = 0;
    bool used = false;
};
using Ring = std::vector<std::pair<long, long>>;
using EdgesFrom = std::multimap<std::pair<long, long>, std::size_t>;

/// Each boundary edge of an inside cell, directed with the inside on its left (counter-
/// clockwise in map orientation).
std::vector<Edge> boundaryEdges(const std::vector<bool>& inside, std::size_t cols, std::size_t rows) {
    const auto in = [&inside, rows, cols](long r, long c) {
        return r >= 0 && c >= 0 && r < static_cast<long>(rows) && c < static_cast<long>(cols) &&
               inside[static_cast<std::size_t>(r) * cols + static_cast<std::size_t>(c)];
    };
    std::vector<Edge> edges;
    for (long r = 0; r < static_cast<long>(rows); ++r) {
        for (long c = 0; c < static_cast<long>(cols); ++c) {
            if (!in(r, c)) continue;
            // y grows southwards here, so counter-clockwise on the map runs: bottom left to
            // bottom right, up the east side, top right to top left, down the west side.
            if (!in(r + 1, c)) edges.emplace_back(c, r + 1, c + 1, r + 1);
            if (!in(r, c + 1)) edges.emplace_back(c + 1, r + 1, c + 1, r);
            if (!in(r - 1, c)) edges.emplace_back(c + 1, r, c, r);
            if (!in(r, c - 1)) edges.emplace_back(c, r, c, r + 1);
        }
    }
    return edges;
}

/// The unused edge continuing `e`, or edges.size(). At a vertex two inside cells touch only by
/// a corner: turn left (in map terms), which keeps them as separate rings.
std::size_t nextEdge(const std::vector<Edge>& edges, const EdgesFrom& from, const Edge& e) {
    const long dx = e.x1 - e.x0;
    const long dy = e.y1 - e.y0;
    std::size_t next = edges.size();
    int bestRank = 9;
    const auto [first, last] = from.equal_range({e.x1, e.y1});
    for (const std::size_t candidate : std::views::values(std::ranges::subrange(first, last))) {
        const Edge& n = edges[candidate];
        if (n.used) continue;
        const long ndx = n.x1 - n.x0;
        const long ndy = n.y1 - n.y0;
        // y points down, so a left turn on the map is (dy, -dx) here.
        int rank = 2;
        if (ndx == dy && ndy == -dx) rank = 0;
        else if (ndx == dx && ndy == dy) rank = 1;
        if (rank < bestRank) {
            bestRank = rank;
            next = candidate;
        }
    }
    return next;
}

/// Chains edges from `start` until the ring closes, marking them used.
Ring traceRing(std::vector<Edge>& edges, const EdgesFrom& from, std::size_t start) {
    Ring ring;
    // The ring closes back at `start`; running out of edges cannot happen on a closed boundary.
    for (std::size_t cur = start; cur != edges.size();) {
        Edge& e = edges[cur];
        e.used = true;
        ring.emplace_back(e.x0, e.y0);
        const bool closed = e.x1 == edges[start].x0 && e.y1 == edges[start].y0;
        cur = closed ? edges.size() : nextEdge(edges, from, e);
    }
    return ring;
}

/// Drops vertices in the middle of straight runs.
Ring simplified(const Ring& ring) {
    Ring simple;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& [px, py] = ring[(i + ring.size() - 1) % ring.size()];
        const auto& [qx, qy] = ring[i];
        const auto& [sx, sy] = ring[(i + 1) % ring.size()];
        if ((qx - px) * (sy - qy) != (qy - py) * (sx - qx)) simple.push_back(ring[i]);
    }
    return simple;
}

/// Signed area in map orientation (x east, y north = -y here): outer rings are positive.
double signedArea(const Ring& r) {
    double a = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& [px, py] = r[i];
        const auto& [qx, qy] = r[(i + 1) % r.size()];
        a += static_cast<double>(px) * static_cast<double>(-qy) - static_cast<double>(qx) * static_cast<double>(-py);
    }
    return a / 2.0;
}

bool ringContains(const Ring& r, double x, double y) {
    bool c = false;
    std::size_t j = r.size() - 1;
    for (std::size_t i = 0; i < r.size(); j = i++) {
        const auto [ix, iy] = r[i];
        const auto [jx, jy] = r[j];
        const auto xi = static_cast<double>(ix);
        const auto yi = static_cast<double>(iy);
        const auto xj = static_cast<double>(jx);
        const auto yj = static_cast<double>(jy);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
    }
    return c;
}

/// The index in `outers` of the smallest outer ring holding the hole, or outers.size().
std::size_t outerOfHole(const std::vector<Ring>& rings, const std::vector<std::size_t>& outers, const Ring& hole) {
    // A point just inside the hole: the middle of its first edge, nudged to the hole side
    // (the hole is on the right of a hole edge, i.e. away from the inside cells).
    const auto [px, py] = hole[0];
    const auto [qx, qy] = hole[1];
    const double mx = (static_cast<double>(px) + static_cast<double>(qx)) / 2.0;
    const double my = (static_cast<double>(py) + static_cast<double>(qy)) / 2.0;
    const auto dx = static_cast<double>(qx - px);
    const auto dy = static_cast<double>(qy - py);
    const double len = std::hypot(dx, dy);
    const double tx = mx - dy / len * 0.25;
    const double ty = my + dx / len * 0.25;
    std::size_t best = outers.size();
    double bestArea = std::numeric_limits<double>::infinity();
    for (std::size_t o = 0; o < outers.size(); ++o) {
        if (const double a = signedArea(rings[outers[o]]); a < bestArea && ringContains(rings[outers[o]], tx, ty)) {
            bestArea = a;
            best = o;
        }
    }
    return best;
}

/// Sets a row counter past the last row when its scope is left by an exception, so the other
/// workers of a coverage calculation take no further rows.
class RowsExhaustedOnThrow {
public:
    RowsExhaustedOnThrow(std::atomic<std::size_t>& nextRow, std::size_t rows)
        : nextRow_(nextRow), rows_(rows) {}
    ~RowsExhaustedOnThrow() {
        if (std::uncaught_exceptions() > unwinding_) nextRow_ = rows_;
    }
    RowsExhaustedOnThrow(const RowsExhaustedOnThrow&) = delete;
    RowsExhaustedOnThrow& operator=(const RowsExhaustedOnThrow&) = delete;
    RowsExhaustedOnThrow(RowsExhaustedOnThrow&&) = delete;
    RowsExhaustedOnThrow& operator=(RowsExhaustedOnThrow&&) = delete;

private:
    std::atomic<std::size_t>& nextRow_;
    std::size_t rows_;
    int unwinding_ = std::uncaught_exceptions();
};

}  // namespace

Calculator::Calculator(LinkService& links, WeatherService& weather, const BuiltinModules& builtins,
                       ModuleRunner& modules, ResultCache& cache, Logger& log)
    : links_(links), weather_(weather), builtins_(builtins), modules_(modules), cache_(cache), log_(log) {}

std::vector<LinkKind> Calculator::kindsFor(const json& mission) {
    const std::string e = mission.value("evaluate", "both");
    std::vector<LinkKind> kinds;
    if (e == "both" || e == "control") kinds.push_back(LinkKind::Control);
    if (e == "both" || e == "video") kinds.push_back(LinkKind::Video);
    for (LinkKind k : kinds) {
        if (!mission.at("links").contains(toString(k))) {
            throw HttpError(422, "evaluate asks for the " + toString(k) + " link but links." + toString(k) + " is not set");
        }
    }
    return kinds;
}

CalcContext Calculator::contextFor(const json& mission) const {
    CalcContext ctx;
    if (const json& t = mission.at("time"); t.value("mode", "now") == "planned") {
        if (!t.contains("utc")) throw HttpError(422, "time.mode is planned but time.utc is missing");
        const auto parsed = parseIsoUtc(t.at("utc").get<std::string>());
        if (!parsed) throw HttpError(422, "time.utc is not an ISO 8601 date-time");
        ctx.time = *parsed;
    } else {
        ctx.time = Clock::now();
    }
    ctx.missionModules = mission.value("modules", json::object());
    if (builtins_.enabled(BuiltinModules::kWeather) && ctx.optedIn(BuiltinModules::kWeather)) {
        const json& p = mission.at("pilot");
        ctx.forecast = weather_.at(p.at("lat").get<double>(), p.at("lon").get<double>(), ctx.time);
    }
    return ctx;
}

json Calculator::environmentKey(const CalcContext& ctx) const {
    const std::optional<double> demError = links_.dem() ? links_.dem()->statedErrorM() : std::nullopt;
    json key = {{"server", links_.version()},
                {"dem", links_.dem() ? links_.dem()->raster().identity() : ""},
                {"demError", demError ? json(*demError) : json()},
                {"landcover", links_.landcover() ? links_.landcover()->raster().identity() : ""},
                {"builtins", builtins_.list()},
                {"modules", modules_.versions(ctx)}};
    // A forecast is an input like any other: a new run gives a new key.
    if (ctx.forecast) key["forecast"] = ctx.forecast->point;
    return key;
}

json Calculator::path(const json& req) {
    CalcContext ctx;
    ctx.time = Clock::now();
    if (req.contains("timeUtc")) {
        const auto t = parseIsoUtc(req.at("timeUtc").get<std::string>());
        if (!t) throw HttpError(422, "timeUtc is not an ISO 8601 date-time");
        ctx.time = *t;
    }
    ctx.missionModules = req.value("modules", json::object());
    const Terminal ground = Terminal::fromJson(req.at("from"));
    const Terminal air = Terminal::fromJson(req.at("to"));
    if (builtins_.enabled(BuiltinModules::kWeather) && ctx.optedIn(BuiltinModules::kWeather)) {
        ctx.forecast = weather_.at(ground.position.latitudeDeg, ground.position.longitudeDeg, ctx.time);
    }

    std::vector<std::pair<LinkKind, ResolvedLink>> links;
    const bool both = req.contains("links");
    if (both) {
        json m = {{"evaluate", req.value("evaluate", "both")}, {"links", req.at("links")}};
        for (LinkKind k : kindsFor(m)) links.emplace_back(k, links_.resolve(req.at("links").at(toString(k)), k));
    } else {
        const LinkKind k = linkKindFromString(req.at("kind").get<std::string>());
        links.emplace_back(k, links_.resolve(req.at("link"), k));
    }

    json refs = json::array();
    for (const auto& [k, l] : links) {
        for (const auto& r : l.refs) addRef(refs, r.toJson());
    }
    const std::string hash = inputHash({{"kind", "path"}, {"request", req}, {"library", refs},
                                        {"time", isoUtc(ctx.time)}, {"env", environmentKey(ctx)}});
    if (auto hit = cache_.get(hash)) return *hit;

    const CalcContextScope scope(ctx);
    const PreparedPath p = links_.prepare(ground, air, ctx);
    if (!p.complete()) throw HttpError(422, "missing elevation data on part of the path; nothing is guessed", links_.missingData(p.missing, p.ground.position, p.air.position));
    json out = json::object();
    for (const auto& [k, l] : links) {
        LinkEvaluation e = links_.evaluate(p, l, ctx, true, true);
        e.result["inputHash"] = hash;
        if (ctx.forecast) e.result["weather"] = ctx.forecast->point;
        e.result["forecastTier"] = forecastTier(ctx);
        out[toString(k)] = std::move(e.result);
    }
    json result = both ? out : out.begin().value();
    cache_.put(hash, "path", result);
    return result;
}

json Calculator::evaluateLeg(const json& mission, std::size_t index, const json& from, const json& to,
                             const std::vector<std::pair<LinkKind, ResolvedLink>>& links, const CalcContext& ctx,
                             std::vector<ModuleFailure>& failures) const {
    const DemSource& dem = links_.requireDem();
    const rfcore::Geodesy geo;
    const Terminal pilot = Terminal::fromJson(mission.at("pilot"));
    const Terminal a = Terminal::fromJson(from);
    const Terminal b = Terminal::fromJson(to);
    const double length = geo.inverse(a.position, b.position).distance.value();

    json leg = {{"index", index}, {"lengthM", length}};
    const std::size_t n = mission.at("waypoints").size();
    leg["from"] = index == 0 ? label(from, "Pilot") : label(from, std::format("WP{}", index));
    leg["to"] = index < n ? label(to, std::format("WP{}", index + 1)) : label(to, "Pilot");

    // Sample positions: the leg's geodesic at the DEM's native step, both ends included. The
    // drone's AGL changes linearly with distance along the leg between the two points' aglM.
    std::vector<rfcore::GeoPoint> pts;
    if (length > 0.0) pts = geo.sample(a.position, b.position, dem.nativeResolution());
    else pts = {a.position};
    const double steps = std::max<double>(1.0, static_cast<double>(pts.size() - 1));

    LegScan scan;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const double f = static_cast<double>(i) / steps;
        const double along = length * f;
        const Terminal drone{pts[i], a.aglM + (b.aglM - a.aglM) * f};
        if (!(geo.inverse(pilot.position, drone.position).distance.value() > 0.0)) {
            ++scan.overPilot;  // the drone is over the pilot: there is no path to evaluate
            continue;
        }
        PreparedPath p = links_.prepare(pilot, drone, ctx);
        if (!p.complete()) {
            recordMissing(scan, links_, p, along);
            continue;
        }
        ++scan.evaluated;
        recordWorst(scan, links_, p, along, links, ctx, failures);
    }
    leg["samples"] = {{"total", pts.size()}, {"evaluated", scan.evaluated}, {"overPilot", scan.overPilot},
                      {"missingData", scan.missingCount}, {"stepM", dem.nativeResolution().value()}};
    if (scan.missingCount > 0) return missingDataLeg(std::move(leg), scan, dem, pts.size());

    std::string status = "ok";
    json reasons = json::array();
    json worstJson = json::object();
    for (const auto& [k, l] : links) {
        const WorstSample& w = scan.worst[k];
        if (!w.path.has_value()) continue;
        LinkEvaluation e = links_.evaluate(*w.path, l, ctx, true, true);
        failures.insert(failures.end(), e.failures.begin(), e.failures.end());
        json& r = e.result;
        r["sample"] = {{"alongLegM", w.along},
                       {"lat", w.path->air.position.latitudeDeg},
                       {"lon", w.path->air.position.longitudeDeg},
                       {"aglM", w.path->air.aglM}};
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
    return leg;
}

void Calculator::runRouteModules(const json& mission, json& result, const CalcContext& ctx) const {
    json soFar = json::object();
    for (const ModuleRunner::Active& a : modules_.optedIn("route.analysis", ctx)) {
        const json req = {{"mission", mission},
                          {"legs", result["legs"]},
                          {"options", a.options},
                          {"modules", soFar},
                          {"weather", ctx.forecast.has_value() ? ctx.forecast->point : json(nullptr)},
                          {"time", isoUtc(ctx.time)}};
        const std::optional<json> answer = analyzeRoute(a, req, result);
        if (!answer.has_value()) continue;
        const json& resp = *answer;
        for (const json& r : resp.value("reasons", json::array())) {
            if (r.is_object()) addRouteReason(result, r, a.id);
        }
        const json data = resp.value("data", json::object());
        result["modules"][a.id] = data;
        soFar[a.id] = data;
        if (resp.contains("battery") && !result.contains("battery")) result["battery"] = resp.at("battery");
    }
}

json Calculator::route(const json& mission, std::optional<int> revision) {
    const CalcContext ctx = contextFor(mission);
    const auto kinds = kindsFor(mission);
    std::vector<std::pair<LinkKind, ResolvedLink>> links;
    json refs = json::array();
    for (LinkKind k : kinds) {
        links.emplace_back(k, links_.resolve(mission.at("links").at(toString(k)), k));
        for (const auto& r : links.back().second.refs) addRef(refs, r.toJson());
    }
    for (const char* kind : {"drones", "batteries"}) {
        const char* field = std::string(kind) == "drones" ? "drone" : "battery";
        if (mission.contains(field)) {
            if (const auto item = links_.library().get(kind, mission.at(field).get<std::string>())) {
                refs.push_back(LibraryRef{kind, item->at("id"), item->at("version"), item->at("verified")}.toJson());
            }
        }
    }
    const std::string hash = inputHash({{"kind", "route"}, {"mission", mission}, {"library", refs},
                                        {"time", isoUtc(ctx.time)}, {"env", environmentKey(ctx)}});
    if (auto hit = cache_.get(hash)) {
        if (revision.has_value()) (*hit)["missionRevision"] = *revision;
        return *hit;
    }

    const CalcContextScope scope(ctx);
    json result = {{"inputHash", hash}, {"legs", json::array()}, {"modules", json::object()},
                   {"moduleFailures", json::array()}, {"reasons", json::array()}, {"library", refs},
                   {"timeUtc", isoUtc(ctx.time)}};
    if (revision.has_value()) result["missionRevision"] = *revision;
    if (ctx.forecast) result["weather"] = ctx.forecast->point;
    result["forecastTier"] = forecastTier(ctx);

    // Legs: pilot -> wp1 -> ... -> last, and back to the pilot when returnToPilot (default true).
    std::vector<json> points{mission.at("pilot")};
    for (const json& w : mission.at("waypoints")) points.push_back(w);
    if (mission.value("returnToPilot", true)) points.push_back(mission.at("pilot"));
    std::vector<ModuleFailure> failures;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        result["legs"].push_back(evaluateLeg(mission, i, points[i], points[i + 1], links, ctx, failures));
    }
    appendModuleFailures(result["moduleFailures"], failures);
    runRouteModules(mission, result, ctx);
    // Only a complete, failure-free result is worth keeping: a module that failed might work next time.
    if (result["moduleFailures"].empty()) cache_.put(hash, "route", result);
    return result;
}

/// The state the worker threads of one coverage calculation share: rows are taken in turn and
/// each cell's margin lands in its own slot.
struct Calculator::CoverageRun {
    CoverageRun(const ResolvedLink& l, const CalcContext& c, const Terminal& p, const CoverageGrid& g,
                CoverageProgress& pr)
        : link(l), ctx(c), pilot(p), grid(g), progress(pr), margins(g.cols * g.rows), worstBand(g.cols * g.rows) {}

    const ResolvedLink& link;
    const CalcContext& ctx;
    const Terminal& pilot;
    const CoverageGrid& grid;
    CoverageProgress& progress;
    std::vector<std::optional<double>> margins;
    std::vector<std::optional<double>> worstBand;
    std::atomic<std::size_t> nextRow{0};
    std::atomic<std::size_t> doneRows{0};
    std::atomic<std::size_t> missingCells{0};
    std::mutex failMutex;
    std::vector<ModuleFailure> failures;
    [[no_unique_address]] const rfcore::Geodesy geo{};
};

void Calculator::coverageWorker(CoverageRun& run) {
    // A failing row ends the whole calculation: its exception reaches coverage() through this
    // worker's future, and the guard hands out the remaining rows so the other workers stop.
    const RowsExhaustedOnThrow guard(run.nextRow, run.grid.rows);
    for (std::size_t r = run.nextRow++; r < run.grid.rows; r = run.nextRow++) {
        if (run.progress.cancelled()) return;
        const ModuleGate::Read gate(modules_.gate());
        const CalcContextScope scope(run.ctx);
        coverageRow(run, r);
        run.progress.report(static_cast<double>(++run.doneRows) / static_cast<double>(run.grid.rows));
    }
}

void Calculator::coverageRow(CoverageRun& run, std::size_t r) const {
    const CoverageGrid& g = run.grid;
    for (std::size_t c = 0; c < g.cols; ++c) {
        const rfcore::GeoPoint centre = g.centre(r, c);
        if (const double d = run.geo.inverse(run.pilot.position, centre).distance.value(); !(d > 0.0) || d > g.radius) {
            continue;
        }
        const PreparedPath p = links_.prepare(run.pilot, Terminal{centre, g.agl}, run.ctx);
        if (!p.complete()) {
            ++run.missingCells;
            continue;
        }
        LinkEvaluation e = links_.evaluate(p, run.link, run.ctx, builtins_.enabled(BuiltinModules::kUncertainty), false);
        run.margins[r * g.cols + c] = e.marginDb;
        if (e.result.contains("band")) run.worstBand[r * g.cols + c] = e.result["band"]["worstDb"].get<double>();
        if (!e.failures.empty()) {
            std::lock_guard lock(run.failMutex);
            run.failures.insert(run.failures.end(), e.failures.begin(), e.failures.end());
        }
    }
}

void Calculator::runCoverageModules(const json& mission, LinkKind kind, const CalcContext& ctx, std::size_t cells,
                                    json& grid) {
    const ModuleGate::Read gate(modules_.gate());
    const CalcContextScope scope(ctx);
    for (const ModuleRunner::Active& a : modules_.optedIn("coverage.analysis", ctx)) {
        const json req = {{"mission", mission},
                          {"grid", grid},
                          {"options", a.options},
                          {"link", toString(kind)},
                          {"time", isoUtc(ctx.time)}};
        const std::string failure = moduleCallFailure([&a, &req, &grid, cells] {
            const json resp = a.instance->invoke("coverage.analyze", req);
            addCoverageModule(grid, resp, a.id, cells);
        });
        if (!failure.empty()) grid["moduleFailures"].push_back({{"module", a.id}, {"error", failure}});
    }
}

json Calculator::coverage(const json& mission, LinkKind kind, CoverageProgress& progress, ThreadPool& pool) {
    const CalcContext ctx = contextFor(mission);
    if (!mission.at("links").contains(toString(kind))) throw HttpError(422, "the mission has no " + toString(kind) + " link");
    const ResolvedLink link = links_.resolve(mission.at("links").at(toString(kind)), kind);
    const json cov = mission.value("coverage", json::object());
    if (!cov.contains("radiusM") || !cov.contains("cellM") || !cov.contains("droneAglM")) {
        throw HttpError(422, "coverage needs coverage.radiusM, coverage.cellM and coverage.droneAglM");
    }
    const double radius = cov.at("radiusM").get<double>();
    const double cell = cov.at("cellM").get<double>();
    const double agl = cov.at("droneAglM").get<double>();
    const Terminal pilot = Terminal::fromJson(mission.at("pilot"));
    json refs = json::array();
    for (const auto& r : link.refs) refs.push_back(r.toJson());
    const std::string hash = inputHash({{"kind", "coverage"}, {"link", toString(kind)}, {"mission", mission},
                                        {"library", refs}, {"time", isoUtc(ctx.time)}, {"env", environmentKey(ctx)}});
    if (auto hit = cache_.get(hash)) {
        (*hit)["cached"] = true;
        return *hit;
    }

    const CoverageGrid g = coverageGrid(pilot.position, radius, cell, agl);
    CoverageRun run(link, ctx, pilot, g, progress);
    std::vector<std::future<void>> parts;
    std::generate_n(std::back_inserter(parts), pool.size(),
                    [this, &pool, &run] { return pool.submit([this, &run] { coverageWorker(run); }); });
    // Every worker must be done with `run` before the first failure is rethrown.
    for (const auto& f : parts) f.wait();
    for (auto& f : parts) f.get();
    if (progress.cancelled()) throw CalculationCancelled();

    json marginJson = json::array();
    json worstJson = json::array();
    std::vector<bool> reachable(g.cols * g.rows, false);
    for (std::size_t i = 0; i < run.margins.size(); ++i) {
        marginJson.push_back(nullable(run.margins[i]));
        worstJson.push_back(nullable(run.worstBand[i]));
        reachable[i] = run.margins[i].value_or(-1.0) >= 0.0;
    }
    const double south = g.north - static_cast<double>(g.rows) * g.dLat;
    const double east = g.west + static_cast<double>(g.cols) * g.dLon;
    json grid = {{"link", toString(kind)},
                 {"origin", {{"lat", g.north}, {"lon", g.west}}},
                 {"pilot", {{"lat", pilot.position.latitudeDeg}, {"lon", pilot.position.longitudeDeg}}},
                 {"cellM", cell},
                 {"cellLatDeg", g.dLat},
                 {"cellLonDeg", g.dLon},
                 {"cols", g.cols},
                 {"rows", g.rows},
                 {"radiusM", radius},
                 {"droneAglM", agl},
                 {"bbox", {g.west, south, east, g.north}},
                 {"marginDb", marginJson},
                 {"limitPolygon", cellsToMultiPolygon(reachable, g.cols, g.rows, g.west, g.north, g.dLon, g.dLat)},
                 {"missingDataCells", run.missingCells.load()},
                 {"library", refs},
                 {"timeUtc", isoUtc(ctx.time)},
                 {"inputHash", hash},
                 {"modules", json::object()},
                 {"reasons", json::array()},
                 {"moduleFailures", json::array()}};
    if (builtins_.enabled(BuiltinModules::kUncertainty)) grid["marginWorstDb"] = worstJson;
    if (ctx.forecast) grid["weather"] = ctx.forecast->point;
    grid["forecastTier"] = forecastTier(ctx);
    appendModuleFailures(grid["moduleFailures"], run.failures);
    runCoverageModules(mission, kind, ctx, g.cols * g.rows, grid);
    if (grid["moduleFailures"].empty()) cache_.put(hash, "coverage", grid);
    (void)log_;
    return grid;
}

std::size_t Calculator::coverageCells(const json& mission) {
    const json cov = mission.value("coverage", json::object());
    if (!cov.contains("radiusM") || !cov.contains("cellM")) return 0;
    const double half = std::ceil(cov.at("radiusM").get<double>() / cov.at("cellM").get<double>());
    const double side = 2.0 * half + 1.0;
    return static_cast<std::size_t>(side * side);
}

json cellsToMultiPolygon(const std::vector<bool>& inside, std::size_t cols, std::size_t rows, double west,
                         double north, double dLon, double dLat) {
    // Boundary edges of the inside cells, chained into rings.
    std::vector<Edge> edges = boundaryEdges(inside, cols, rows);
    EdgesFrom from;
    for (std::size_t i = 0; i < edges.size(); ++i) from.insert({{edges[i].x0, edges[i].y0}, i});
    std::vector<Ring> rings;
    for (std::size_t start = 0; start < edges.size(); ++start) {
        if (!edges[start].used) rings.push_back(simplified(traceRing(edges, from, start)));
    }

    std::vector<std::size_t> outers;
    std::vector<std::vector<std::size_t>> holesOf(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (signedArea(rings[i]) > 0.0) outers.push_back(i);
    }
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (signedArea(rings[i]) > 0.0 || rings[i].size() < 2) continue;
        if (const std::size_t best = outerOfHole(rings, outers, rings[i]); best < outers.size()) holesOf[outers[best]].push_back(i);
    }
    const auto coords = [west, north, dLon, dLat](const Ring& r) {
        json ring = json::array();
        for (const auto& [x, y] : r) {
            ring.push_back({west + static_cast<double>(x) * dLon, north - static_cast<double>(y) * dLat});
        }
        if (!ring.empty()) ring.push_back(ring.front());
        return ring;
    };
    json polys = json::array();
    for (std::size_t o : outers) {
        json poly = json::array({coords(rings[o])});
        for (std::size_t h : holesOf[o]) poly.push_back(coords(rings[h]));
        polys.push_back(poly);
    }
    return {{"type", "MultiPolygon"}, {"coordinates", polys}};
}

}  // namespace rfmap
