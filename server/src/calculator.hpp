#pragma once

#include "builtins.hpp"
#include "common.hpp"
#include "coverage_engine.hpp"
#include "invocation_scope.hpp"
#include "link_service.hpp"
#include "module_runner.hpp"
#include "result_cache.hpp"
#include "result_key.hpp"
#include "route_evaluator.hpp"
#include "schemas.hpp"
#include "thread_pool.hpp"
#include "weather.hpp"

namespace rfmap {

/// The three calculations of the API: one path, a whole route, a coverage grid. Every result
/// carries the hash of all its inputs and is cached under it. The route and coverage work is
/// done by RouteEvaluator and CoverageEngine; this class makes the context, checks the cache
/// and opens the InvocationScope the modules work in.
class Calculator final : public IContextFactory {
public:
    Calculator(LinkService& links, WeatherService& weather, const BuiltinModules& builtins, const ModuleRunner& modules,
               ResultCache& cache, const SchemaRegistry& schemas);

    /// Time, module choices and forecast for a mission. "planned" without utc is a 422; module
    /// options that do not match the module's schema are a 422 too.
    [[nodiscard]] CalcContext contextFor(const json& mission) const override;

    /// POST /calc/path. Single link ({kind, link}) → LinkResult; both ({links, evaluate}) →
    /// {control?, video?}. 422 with MissingData when the DEM has a gap on the path.
    json path(const json& request);

    /// POST /calc/route → RouteResult. `revision` is the stored mission's revision, if known.
    json route(const json& mission, std::optional<int> revision);

    /// One link's coverage grid. `progress` gets 0..1 and is polled for cancelling between rows.
    json coverage(const json& mission, LinkKind kind, CoverageProgress& progress, ThreadPool& pool);
    /// Cells a coverage request would compute, for the size limit.
    [[nodiscard]] static std::size_t coverageCells(const json& mission) { return CoverageGrid::cellsFor(mission); }

    [[nodiscard]] static std::vector<LinkKind> kindsFor(const json& mission);

    /// The services a module's host calls reach while this calculator runs.
    [[nodiscard]] const HostEnvironment& hostEnvironment() const { return environment_; }

private:
    /// The context's module choices, checked against the installed modules' option schemas.
    [[nodiscard]] CalcContext withModules(CalcContext ctx, const json& missionModules, const json& pilot) const;
    [[nodiscard]] RouteEvaluator::Links resolveLinks(const json& links, const std::vector<LinkKind>& kinds) const;

    LinkService& links_;
    WeatherService& weather_;
    const BuiltinModules& builtins_;
    const ModuleRunner& modules_;
    ResultCache& cache_;
    ResultKeyBuilder keys_;
    RouteEvaluator routes_;
    CoverageEngine coverage_;
    HostEnvironment environment_;
};

}  // namespace rfmap
