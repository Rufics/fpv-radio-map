#pragma once

#include "builtins.hpp"
#include "common.hpp"
#include "link_service.hpp"
#include "logger.hpp"
#include "module_runner.hpp"
#include "result_cache.hpp"
#include "thread_pool.hpp"
#include "weather.hpp"

#include <atomic>
#include <stdexcept>

namespace rfmap {

/// Where a coverage calculation reports its progress and learns that it should stop.
class CoverageProgress {
public:
    virtual ~CoverageProgress() = default;
    /// Called with 0..1 as rows finish, from the calculation's worker threads.
    virtual void report(double fraction) = 0;
    /// Polled between rows; true stops the calculation.
    [[nodiscard]] virtual bool cancelled() const = 0;
};

/// Thrown when a coverage calculation stops because its CoverageProgress asked it to.
class CalculationCancelled : public std::runtime_error {
public:
    CalculationCancelled() : std::runtime_error("cancelled") {}
};

/// The three calculations of the API: one path, a whole route, a coverage grid. Every result
/// carries the hash of all its inputs and is cached under it.
class Calculator {
public:
    Calculator(LinkService& links, WeatherService& weather, const BuiltinModules& builtins, ModuleRunner& modules,
               ResultCache& cache, Logger& log);

    /// Time, module choices and forecast for a mission. "planned" without utc is a 422.
    [[nodiscard]] CalcContext contextFor(const json& mission) const;

    /// POST /calc/path. Single link ({kind, link}) → LinkResult; both ({links, evaluate}) →
    /// {control?, video?}. 422 with MissingData when the DEM has a gap on the path.
    json path(const json& request);

    /// POST /calc/route → RouteResult. `revision` is the stored mission's revision, if known.
    json route(const json& mission, std::optional<int> revision);

    /// One link's coverage grid. `progress` gets 0..1 and is polled for cancelling between rows.
    json coverage(const json& mission, LinkKind kind, CoverageProgress& progress, ThreadPool& pool);
    /// Cells a coverage request would compute, for the size limit.
    [[nodiscard]] static std::size_t coverageCells(const json& mission);

    /// The cache key inputs shared by all three: terrain files, built-in switches, module
    /// versions, server version.
    [[nodiscard]] json environmentKey(const CalcContext& ctx) const;

    [[nodiscard]] static std::vector<LinkKind> kindsFor(const json& mission);

private:
    json evaluateLeg(const json& mission, std::size_t index, const json& from, const json& to,
                     const std::vector<std::pair<LinkKind, ResolvedLink>>& links, const CalcContext& ctx,
                     std::vector<ModuleFailure>& failures) const;
    void runRouteModules(const json& mission, json& result, const CalcContext& ctx) const;

    struct CoverageRun;
    /// One pool thread of a coverage calculation: takes rows until none are left.
    void coverageWorker(CoverageRun& run);
    void coverageRow(CoverageRun& run, std::size_t row) const;
    void runCoverageModules(const json& mission, LinkKind kind, const CalcContext& ctx, std::size_t cells, json& grid);

    LinkService& links_;
    WeatherService& weather_;
    const BuiltinModules& builtins_;
    ModuleRunner& modules_;
    ResultCache& cache_;
    Logger& log_;
};

/// GeoJSON MultiPolygon of the cells set in `inside` (row-major, row 0 north): every ring
/// follows cell edges exactly, so the line is the grid's, not an interpolation.
json cellsToMultiPolygon(const std::vector<bool>& inside, std::size_t cols, std::size_t rows, double west,
                         double north, double dLon, double dLat);

}  // namespace rfmap
