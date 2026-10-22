#pragma once

#include "common.hpp"
#include "file_lock.hpp"
#include "link_service.hpp"

#include <rfcore/link_budget.hpp>
#include <rfmod/registry.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace rfmap {

/// Set while a module answers loss.contribute on this thread. link.evaluate is refused inside
/// it: the evaluation would call loss.contribute again and never end.
class LossContributionScope {
public:
    explicit LossContributionScope(std::string moduleId);
    ~LossContributionScope();
    LossContributionScope(const LossContributionScope&) = delete;
    LossContributionScope& operator=(const LossContributionScope&) = delete;
    /// The module currently contributing on this thread, or empty.
    static const std::string& active();

private:
    std::string previous_;
};

/// Where the two ends of an evaluated path are, for the module request.
struct PathEnds {
    Terminal tx;
    Terminal rx;
};

/// A module's loss.contribute as an rf-core loss contributor. Failures are recorded and
/// contribute nothing; terms whose id does not start with "<moduleId>/" are dropped.
class ModuleLossContributor final : public rfcore::ILossContributor {
public:
    ModuleLossContributor(rfmod::IModuleInstance& instance, json options, const PathEnds& ends, TimePoint time,
                          const LandCoverSource* landcover, std::vector<ModuleFailure>& failures);

    std::vector<rfcore::BudgetTerm> contribute(const rfcore::PathContext& context) const override;
    std::vector<rfcore::UncertaintyNote> notes(const rfcore::PathContext& context) const override;

    /// The term-prefix rule. Malformed terms are dropped too; `dropped` lists why.
    static std::vector<rfcore::BudgetTerm> acceptTerms(const std::string& moduleId, const json& terms,
                                                       std::vector<std::string>& dropped);

private:
    json request(const rfcore::PathContext& context) const;

    rfmod::IModuleInstance& module_;
    std::string id_;
    json options_;
    PathEnds ends_;
    TimePoint time_;
    const LandCoverSource* landcover_;
    std::vector<ModuleFailure>& failures_;
    // What the last contribute() returned, for notes(); both are const calls from rf-core.
    mutable std::mutex lastMutex_;
    mutable const rfcore::TerrainProfile* lastProfile_ = nullptr;
    mutable std::vector<rfcore::UncertaintyNote> lastNotes_;
};

/// The installed rfmod modules as the calculations see them: active, providing a capability,
/// and opted in by the mission. Admin changes take the gate exclusively, so instance pointers
/// stay valid for a request holding it shared.
class ModuleRunner {
public:
    explicit ModuleRunner(rfmod::ModuleRegistry* registry) : registry_(registry) {}

    struct Active {
        rfmod::IModuleInstance* instance;
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

    ModuleGate& gate() { return gate_; }
    void setRegistry(rfmod::ModuleRegistry* r) { registry_ = r; }
    [[nodiscard]] rfmod::ModuleRegistry* registry() const { return registry_; }

private:
    rfmod::ModuleRegistry* registry_;
    ModuleGate gate_;
};

}  // namespace rfmap
