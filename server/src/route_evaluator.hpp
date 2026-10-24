#pragma once

#include "link_service.hpp"
#include "module_runner.hpp"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace rfmap {

/// The route calculation: every leg sampled at the DEM's step, the worst sample of each link
/// kept, then the route.analysis modules. Runs inside the caller's InvocationScope.
class RouteEvaluator {
public:
    using Links = std::vector<std::pair<LinkKind, ResolvedLink>>;

    RouteEvaluator(LinkService& links, const ModuleRunner& modules);

    /// The legs and modules of `result`, which already carries its header fields.
    void evaluate(const json& mission, const Links& links, const CalcContext& ctx, json& result) const;

private:
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
        std::size_t belowGround = 0;
        double lowestAglM = 0.0;    ///< the deepest position below the ground
        double lowestAlongM = 0.0;
        json missing = json::array();
        json missingBbox = nullptr;
    };
    /// What every sample of a leg is evaluated with.
    struct LegContext {
        const Links& links;
        const CalcContext& ctx;
        std::vector<ModuleFailure>& failures;
        bool autonomous;  ///< mission flightMode autonomous: the autopilot flies without the links
    };

    [[nodiscard]] json evaluateLeg(const json& mission, std::size_t index, const json& from, const json& to,
                                   const Links& links, const CalcContext& ctx, std::vector<ModuleFailure>& failures) const;
    /// Evaluates one drone position on a leg, or records why it was not.
    void scanSample(LegScan& scan, const Terminal& pilot, const Terminal& drone, double along, const LegContext& lc) const;
    /// Without links to evaluate, only the ground under the drone is needed: records whether the
    /// DEM has it.
    void scanGround(LegScan& scan, const rfcore::GeoPoint& position, double along) const;
    /// Records a drone position the plan puts below the ground.
    static void recordBelowGround(LegScan& scan, double aglM, double along);
    /// Marks a leg whose plan goes below the ground as blocked, with the reason.
    static void markBelowGround(json& leg, const LegScan& scan, std::size_t total);
    /// Records a drone position whose path from the pilot lacks terrain data; the first 20 are listed.
    void recordMissing(LegScan& scan, const PreparedPath& p, double along) const;
    /// Evaluates every link at one sample position and keeps each link's worst.
    void recordWorst(LegScan& scan, const PreparedPath& p, double along, const Links& links, const CalcContext& ctx,
                     std::vector<ModuleFailure>& failures) const;
    /// The full evaluation of each link's worst sample, with the leg's status and reasons.
    void describeWorst(json& leg, const LegScan& scan, const LegContext& lc) const;
    /// Marks a leg with terrain gaps as not evaluated.
    [[nodiscard]] static json missingDataLeg(json leg, const LegScan& scan, const DemSource& dem, std::size_t total);
    void runModules(const json& mission, json& result, const CalcContext& ctx) const;
    /// Calls a module's route.analyze; a failure is listed in result.moduleFailures and gives nothing.
    [[nodiscard]] static std::optional<json> analyze(const ModuleRunner::Active& a, const json& req, json& result);
    /// A module's reason with "leg": i belongs to that leg; the rest to the route.
    static void addReason(json& result, json r, const std::string& moduleId);

    LinkService& links_;
    const ModuleRunner& modules_;
};

}  // namespace rfmap
