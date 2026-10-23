#pragma once

#include "common.hpp"
#include "link_service.hpp"

#include <rfcore/link_budget.hpp>
#include <rfmod/registry.hpp>

#include <memory>
#include <string>
#include <vector>

namespace rfmap {

/// Where the two ends of an evaluated path are, for the module request.
struct PathEnds {
    Terminal tx;
    Terminal rx;
};

/// A module's loss.contribute as an rf-core loss contributor. Failures are recorded and
/// contribute nothing; terms whose id does not start with "<moduleId>/" are dropped.
class ModuleLossContributor final : public rfcore::ILossContributor {
public:
    ModuleLossContributor(std::shared_ptr<rfmod::IModuleInstance> instance, json options, const PathEnds& ends,
                          TimePoint time, const LandCoverSource* landcover, std::vector<ModuleFailure>& failures);

    [[nodiscard]] rfcore::LossContribution contribute(const rfcore::PathContext& context) const override;

    /// The term-prefix rule. Malformed terms are dropped too; `dropped` lists why.
    static std::vector<rfcore::BudgetTerm> acceptTerms(const std::string& moduleId, const json& terms,
                                                       std::vector<std::string>& dropped);

private:
    [[nodiscard]] json request(const rfcore::PathContext& context) const;
    /// The module's answer as a contribution; the dropped terms become notes.
    [[nodiscard]] rfcore::LossContribution accept(const json& response) const;

    std::shared_ptr<rfmod::IModuleInstance> module_;
    std::string id_;
    json options_;
    PathEnds ends_;
    TimePoint time_;
    const LandCoverSource* landcover_;
    std::vector<ModuleFailure>& failures_;
};

/// The installed rfmod modules as the calculations see them: active, providing a capability,
/// and opted in by the mission. Instances are shared with the registry, so a calculation keeps
/// the module it started with even if the admin reloads the registry meanwhile.
class ModuleRunner {
public:
    explicit ModuleRunner(const rfmod::ModuleRegistry& registry) : registry_(registry) {}

    struct Active {
        std::shared_ptr<rfmod::IModuleInstance> instance;
        std::string id;
        json options;
    };
    /// In registry dependency order.
    [[nodiscard]] std::vector<Active> optedIn(const std::string& capability, const CalcContext& ctx) const;

    [[nodiscard]] std::vector<std::shared_ptr<const rfcore::ILossContributor>> lossContributors(
        const CalcContext& ctx, const PathEnds& ends, const LandCoverSource* landcover,
        std::vector<ModuleFailure>& failures) const;

    /// Versions of opted-in modules, for result cache keys.
    [[nodiscard]] json versions(const CalcContext& ctx) const;

    /// Why the mission's per-module options are not acceptable, for HTTP 422; empty when they
    /// are. Options for a package that is not installed or not active are reported too; the
    /// server's own rf.* modules are not checked here.
    [[nodiscard]] std::vector<std::string> checkOptions(const json& missionModules) const;

    [[nodiscard]] const rfmod::ModuleRegistry& registry() const { return registry_; }

private:
    const rfmod::ModuleRegistry& registry_;
};

}  // namespace rfmap
