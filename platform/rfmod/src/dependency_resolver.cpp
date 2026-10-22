#include "rfmod/dependency_resolver.hpp"

#include <algorithm>
#include <ranges>

namespace rfmod {

std::string toString(ModuleState s) {
    using enum ModuleState;
    switch (s) {
        case Active: return "active";
        case Disabled: return "disabled";
        case MissingDependency: return "missing-dependency";
        case Failed: return "failed";
    }
    return "failed";
}

DependencyResolver::DependencyResolver(const Candidates& candidates, const Version& hostVersion)
    : candidates_(candidates), hostVersion_(hostVersion) {}

std::vector<DependencyResolver::Step> DependencyResolver::plan() {
    for (const auto& id : candidates_ | std::views::keys) (void)visit(id);
    return std::move(steps_);
}

bool DependencyResolver::loadable(const std::string& id) const {
    const auto it = std::ranges::find(steps_, id, &Step::id);
    return it != steps_.end() && it->state == ModuleState::Active;
}

bool DependencyResolver::visit(const std::string& id) {
    const auto mark = marks_.find(id);
    if (mark != marks_.end() && mark->second == Mark::Done) return loadable(id);
    if (mark != marks_.end()) {
        // Visiting: the dependency chain came back to this module.
        steps_.emplace_back(id, ModuleState::Failed, "dependency cycle through " + id);
        marks_[id] = Mark::Done;
        return false;
    }
    marks_[id] = Mark::Visiting;
    const Candidate& c = candidates_.at(id);
    Step step{id, ModuleState::Active, {}};
    if (mayLoad(c, step) && requiredDependenciesLoadable(c, step)) visitOptionalDependencies(c);
    // A cycle may already have appended a step for this id.
    if (std::ranges::find(steps_, id, &Step::id) == steps_.end()) steps_.push_back(std::move(step));
    marks_[id] = Mark::Done;
    return loadable(id);
}

bool DependencyResolver::mayLoad(const Candidate& c, Step& step) const {
    if (!c.enabled) {
        step.state = ModuleState::Disabled;
        step.detail = "disabled by the admin";
        return false;
    }
    if (c.manifest->deprecation && hostVersion_ >= c.manifest->deprecation->removedIn) {
        step.state = ModuleState::Failed;
        step.detail = "removed in host " + c.manifest->deprecation->removedIn.str();
        return false;
    }
    return true;
}

bool DependencyResolver::requiredDependenciesLoadable(const Candidate& c, Step& step) {
    for (const Dependency& d : c.manifest->dependencies) {
        const auto dep = candidates_.find(d.id);
        const bool installed = dep != candidates_.end();
        if (installed && d.range.contains(dep->second.manifest->version) && visit(d.id)) continue;
        step.state = ModuleState::MissingDependency;
        step.detail = "needs " + d.id + " " + d.range.str();
        if (!installed) {
            step.detail += " (not installed)";
        } else {
            const auto planned = std::ranges::find(steps_, d.id, &Step::id);
            step.detail += " (installed " + dep->second.manifest->version.str() + ", " +
                           toString(planned == steps_.end() ? ModuleState::Failed : planned->state) + ")";
        }
        return false;
    }
    return true;
}

void DependencyResolver::visitOptionalDependencies(const Candidate& c) {
    for (const Dependency& d : c.manifest->optionalDependencies) {
        const auto dep = candidates_.find(d.id);
        if (dep != candidates_.end() && d.range.contains(dep->second.manifest->version)) (void)visit(d.id);
    }
}

}  // namespace rfmod
