#include "rfmod/dependency_resolver.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <string_view>

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
    using enum Mark;
    const auto mark = marks_.find(id);
    if (mark != marks_.end() && mark->second == Done) return loadable(id);
    // Visiting: the dependency chain came back to this module. The caller that closed the
    // cycle reports it; this module's own step is decided when its visit unwinds.
    if (mark != marks_.end()) return false;
    marks_[id] = Visiting;
    const Candidate& c = candidates_.at(id);
    Step step{id, ModuleState::Active, {}};
    if (mayLoad(c, step) && requiredDependenciesLoadable(c, step)) visitOptionalDependencies(c);
    // A cycle may already have appended a step for this id.
    if (std::ranges::find(steps_, id, &Step::id) == steps_.end()) steps_.push_back(std::move(step));
    marks_[id] = Done;
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
        if (installed && visiting(d.id)) {
            step.state = ModuleState::Failed;
            step.detail = "dependency cycle through " + d.id;
            return false;
        }
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
        if (const auto dep = candidates_.find(d.id); dep == candidates_.end() || !d.range.contains(dep->second.manifest->version)) {
            continue;
        }
        // An optional dependency that is on the chain, or requires a module on it, is no cycle to
        // fail on: it is planned after the chain unwinds, and both modules load.
        std::set<std::string_view> seen;
        if (!requiresVisiting(d.id, seen)) (void)visit(d.id);
    }
}

bool DependencyResolver::visiting(const std::string& id) const {
    const auto mark = marks_.find(id);
    return mark != marks_.end() && mark->second == Mark::Visiting;
}

bool DependencyResolver::requiresVisiting(const std::string& id, std::set<std::string_view>& seen) const {
    if (visiting(id)) return true;
    const auto candidate = candidates_.find(id);
    if (candidate == candidates_.end() || !seen.insert(id).second) return false;
    return std::ranges::any_of(candidate->second.manifest->dependencies,
                               [this, &seen](const Dependency& d) { return requiresVisiting(d.id, seen); });
}

}  // namespace rfmod
