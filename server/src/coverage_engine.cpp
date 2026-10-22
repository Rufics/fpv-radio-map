#include "coverage_engine.hpp"

#include "grid_outline.hpp"
#include "invocation_scope.hpp"
#include "module_call.hpp"

#include <rfcore/constants.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/geodesy.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <future>
#include <iterator>
#include <mutex>

namespace rfmap {

namespace {

json nullable(const std::optional<double>& v) { return v.has_value() ? json(*v) : json(nullptr); }

/// Sets a row counter past the last row when its scope is left by an exception, so the other
/// workers of a coverage calculation take no further rows.
class RowsExhaustedOnThrow {
public:
    RowsExhaustedOnThrow(std::atomic<std::size_t>& nextRow, std::size_t rows) : nextRow_(nextRow), rows_(rows) {}
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

CoverageGrid::CoverageGrid(const rfcore::GeoPoint& pilot, double radiusM, double cellM, double aglM)
    : radius_(radiusM), cell_(cellM), agl_(aglM) {
    const double lat0 = pilot.latitudeDeg;
    const double deg = rfcore::constants::kPi / 180.0;
    const double m = rfcore::EarthModel::radiusOfCurvature(lat0, 0.0).value();
    const double nRad = rfcore::EarthModel::radiusOfCurvature(lat0, 90.0).value();
    dLat_ = cellM / (m * deg);
    dLon_ = cellM / (nRad * std::cos(lat0 * deg) * deg);
    const auto half = static_cast<std::size_t>(std::ceil(radiusM / cellM));
    cols_ = 2 * half + 1;
    rows_ = 2 * half + 1;
    north_ = lat0 + (static_cast<double>(half) + 0.5) * dLat_;
    west_ = pilot.longitudeDeg - (static_cast<double>(half) + 0.5) * dLon_;
}

rfcore::GeoPoint CoverageGrid::centre(std::size_t row, std::size_t col) const {
    return {north_ - (static_cast<double>(row) + 0.5) * dLat_, west_ + (static_cast<double>(col) + 0.5) * dLon_};
}

std::size_t CoverageGrid::cellsFor(const json& mission) {
    const json cov = mission.value("coverage", json::object());
    if (!cov.contains("radiusM") || !cov.contains("cellM")) return 0;
    const double half = std::ceil(cov.at("radiusM").get<double>() / cov.at("cellM").get<double>());
    const double side = 2.0 * half + 1.0;
    return static_cast<std::size_t>(side * side);
}

/// The state the worker threads of one coverage calculation share: rows are taken in turn and
/// each cell's margin lands in its own slot.
struct CoverageEngine::Run {
    Run(const Request& r, const Terminal& p, const CoverageGrid& g, CoverageProgress& pr, const InvocationScope& s)
        : request(r), pilot(p), grid(g), progress(pr), scope(s), margins(g.cells()), worstBand(g.cells()) {}

    const Request& request;
    const Terminal& pilot;
    const CoverageGrid& grid;
    CoverageProgress& progress;
    const InvocationScope& scope;  ///< the request's scope, which each worker thread joins
    std::vector<std::optional<double>> margins;
    std::vector<std::optional<double>> worstBand;
    std::atomic<std::size_t> nextRow{0};
    std::atomic<std::size_t> doneRows{0};
    std::atomic<std::size_t> missingCells{0};
    std::mutex failMutex;
    std::vector<ModuleFailure> failures;
    [[no_unique_address]] const rfcore::Geodesy geo{};
};

CoverageEngine::CoverageEngine(LinkService& links, const BuiltinModules& builtins, const ModuleRunner& modules)
    : links_(links), builtins_(builtins), modules_(modules) {}

void CoverageEngine::worker(Run& run) const {
    // A failing row ends the whole calculation: its exception reaches run() through this
    // worker's future, and the guard hands out the remaining rows so the other workers stop.
    const RowsExhaustedOnThrow guard(run.nextRow, run.grid.rows());
    // Modules called from this thread work for the same request, on the same host-call budget,
    // as the thread that started it.
    const InvocationScope scope(InvocationScope::FromThread{}, run.scope);
    for (std::size_t r = run.nextRow++; r < run.grid.rows(); r = run.nextRow++) {
        if (run.progress.cancelled()) return;
        row(run, r);
        run.progress.report(static_cast<double>(++run.doneRows) / static_cast<double>(run.grid.rows()));
    }
}

void CoverageEngine::row(Run& run, std::size_t r) const {
    const CoverageGrid& g = run.grid;
    for (std::size_t c = 0; c < g.cols(); ++c) {
        const rfcore::GeoPoint centre = g.centre(r, c);
        if (const double d = run.geo.inverse(run.pilot.position, centre).distance.value(); !(d > 0.0) || d > g.radiusM()) {
            continue;
        }
        const PreparedPath p = links_.prepare(run.pilot, Terminal{centre, g.aglM()}, run.request.ctx);
        if (!p.complete()) {
            ++run.missingCells;
            continue;
        }
        LinkEvaluation e = links_.evaluate(p, run.request.link, run.request.ctx,
                                           builtins_.enabled(BuiltinModules::kUncertainty), false);
        run.margins[r * g.cols() + c] = e.marginDb;
        if (e.result.contains("band")) run.worstBand[r * g.cols() + c] = e.result["band"]["worstDb"].get<double>();
        if (!e.failures.empty()) {
            const std::lock_guard lock(run.failMutex);
            run.failures.insert(run.failures.end(), e.failures.begin(), e.failures.end());
        }
    }
}

void CoverageEngine::addModule(json& grid, const json& resp, const std::string& moduleId, std::size_t cells) {
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

void CoverageEngine::runModules(const Request& request, std::size_t cells, json& grid) const {
    for (const ModuleRunner::Active& a : modules_.optedIn("coverage.analysis", request.ctx)) {
        const json req = {{"mission", request.mission},
                          {"grid", grid},
                          {"options", a.options},
                          {"link", toString(request.kind)},
                          {"time", UtcTime::format(request.ctx.time)}};
        const std::string failure = ModuleCall::failure([&a, &req, &grid, cells] {
            const json resp = a.instance->invoke("coverage.analyze", req);
            addModule(grid, resp, a.id, cells);
        });
        if (!failure.empty()) grid["moduleFailures"].push_back({{"module", a.id}, {"error", failure}});
    }
}

json CoverageEngine::gridJson(const Request& request, const CoverageGrid& g, const Run& run) const {
    json marginJson = json::array();
    json worstJson = json::array();
    std::vector<bool> reachable(g.cells(), false);
    for (std::size_t i = 0; i < run.margins.size(); ++i) {
        marginJson.push_back(nullable(run.margins[i]));
        worstJson.push_back(nullable(run.worstBand[i]));
        reachable[i] = run.margins[i].value_or(-1.0) >= 0.0;
    }
    const PolygonTracer tracer(g.cols(), g.rows(), g.west(), g.north(), g.dLon(), g.dLat());
    json grid = {{"link", toString(request.kind)},
                 {"origin", {{"lat", g.north()}, {"lon", g.west()}}},
                 {"pilot", {{"lat", run.pilot.position.latitudeDeg}, {"lon", run.pilot.position.longitudeDeg}}},
                 {"cellM", g.cellM()},
                 {"cellLatDeg", g.dLat()},
                 {"cellLonDeg", g.dLon()},
                 {"cols", g.cols()},
                 {"rows", g.rows()},
                 {"radiusM", g.radiusM()},
                 {"droneAglM", g.aglM()},
                 {"bbox", {g.west(), g.south(), g.east(), g.north()}},
                 {"marginDb", marginJson},
                 {"limitPolygon", tracer.multiPolygon(reachable)},
                 {"missingDataCells", run.missingCells.load()},
                 {"library", request.libraryRefs},
                 {"timeUtc", UtcTime::format(request.ctx.time)},
                 {"inputHash", request.hash},
                 {"modules", json::object()},
                 {"reasons", json::array()},
                 {"moduleFailures", json::array()}};
    if (builtins_.enabled(BuiltinModules::kUncertainty)) grid["marginWorstDb"] = worstJson;
    if (request.ctx.forecast) grid["weather"] = request.ctx.forecast->point;
    grid["forecastTier"] = request.ctx.forecastTier();
    ModuleFailure::appendAll(grid["moduleFailures"], run.failures);
    return grid;
}

json CoverageEngine::run(const Request& request, CoverageProgress& progress, ThreadPool& pool) const {
    const json cov = request.mission.value("coverage", json::object());
    if (!cov.contains("radiusM") || !cov.contains("cellM") || !cov.contains("droneAglM")) {
        throw HttpError(422, "coverage needs coverage.radiusM, coverage.cellM and coverage.droneAglM");
    }
    const Terminal pilot = Terminal::fromJson(request.mission.at("pilot"));
    const CoverageGrid g(pilot.position, cov.at("radiusM").get<double>(), cov.at("cellM").get<double>(),
                         cov.at("droneAglM").get<double>());
    Run run(request, pilot, g, progress, InvocationScope::require());
    std::vector<std::future<void>> parts;
    std::generate_n(std::back_inserter(parts), pool.size(),
                    [this, &pool, &run] { return pool.submit([this, &run] { worker(run); }); });
    // Every worker must be done with `run` before the first failure is rethrown.
    for (const auto& f : parts) f.wait();
    for (auto& f : parts) f.get();
    if (progress.cancelled()) throw CalculationCancelled();

    json grid = gridJson(request, g, run);
    runModules(request, g.cells(), grid);
    return grid;
}

}  // namespace rfmap
