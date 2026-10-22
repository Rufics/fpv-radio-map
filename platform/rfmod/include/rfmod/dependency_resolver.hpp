#pragma once

#include "rfmod/manifest.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rfmod {

enum class ModuleState { Active, Disabled, MissingDependency, Failed };
std::string toString(ModuleState s);

/// Orders installed modules so that every module comes after what it requires, and says which
/// ones cannot be loaded at all: disabled, removed in this host version, missing or out-of-range
/// dependencies, cycles. Pure bookkeeping: it loads nothing.
class DependencyResolver {
public:
    struct Candidate {
        const Manifest* manifest;
        bool enabled;
    };
    using Candidates = std::map<std::string, Candidate, std::less<>>;

    /// One module's place in the plan. `state` Active means "load it"; the registry may still
    /// turn it into Failed or MissingDependency once the loads before it have run.
    struct Step {
        std::string id;
        ModuleState state = ModuleState::Disabled;
        std::string detail;
    };

    DependencyResolver(const Candidates& candidates, const Version& hostVersion);

    /// Every candidate once, in load order.
    [[nodiscard]] std::vector<Step> plan();

private:
    enum class Mark { New, Visiting, Done };

    /// Depth-first over required and optional dependencies; the step for `id` is appended after
    /// its dependencies' steps. Returns whether `id` ends up loadable.
    bool visit(const std::string& id);
    bool mayLoad(const Candidate& c, Step& step) const;
    bool requiredDependenciesLoadable(const Candidate& c, Step& step);
    void visitOptionalDependencies(const Candidate& c);
    /// True while `id` is on the dependency chain being walked.
    [[nodiscard]] bool visiting(const std::string& id) const;
    /// The already planned step of `id`, if any, is loadable.
    [[nodiscard]] bool loadable(const std::string& id) const;

    const Candidates& candidates_;
    Version hostVersion_;
    std::map<std::string, Mark, std::less<>> marks_;
    std::vector<Step> steps_;
};

}  // namespace rfmod
