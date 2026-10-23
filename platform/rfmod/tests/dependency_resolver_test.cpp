#include "rfmod/dependency_resolver.hpp"

#include <gtest/gtest.h>

#include <map>
#include <ranges>
#include <string>
#include <utility>

using namespace rfmod;

namespace {

Manifest manifestOf(const std::string& id, const std::string& version, const std::vector<std::pair<std::string, std::string>>& deps = {}) {
    Manifest m;
    m.id = id;
    m.version = Version::parse(version);
    for (const auto& [dep, range] : deps) m.dependencies.emplace_back(dep, VersionRange::parse(range));
    return m;
}

std::map<std::string, DependencyResolver::Step> byId(const std::vector<DependencyResolver::Step>& steps) {
    std::map<std::string, DependencyResolver::Step> out;
    for (const auto& s : steps) out[s.id] = s;
    return out;
}

}  // namespace

TEST(DependencyResolver, OrdersDependenciesFirst) {
    const Manifest a = manifestOf("a", "1.0.0");
    const Manifest b = manifestOf("b", "1.0.0", {{"a", "^1.0.0"}});
    const Manifest c = manifestOf("c", "1.0.0", {{"b", "^1.0.0"}});
    DependencyResolver::Candidates candidates = {{"c", {&c, true}}, {"b", {&b, true}}, {"a", {&a, true}}};
    const auto steps = DependencyResolver(candidates, Version(0, 1, 0)).plan();
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0].id, "a");
    EXPECT_EQ(steps[1].id, "b");
    EXPECT_EQ(steps[2].id, "c");
    for (const auto& s : steps) EXPECT_EQ(s.state, ModuleState::Active) << s.id;
}

TEST(DependencyResolver, MissingOutOfRangeAndDisabledDependencies) {
    const Manifest a = manifestOf("a", "2.0.0");
    const Manifest b = manifestOf("b", "1.0.0", {{"a", "^1.0.0"}});
    const Manifest c = manifestOf("c", "1.0.0", {{"nobody", "*"}});
    const Manifest d = manifestOf("d", "1.0.0", {{"e", "*"}});
    const Manifest e = manifestOf("e", "1.0.0");
    DependencyResolver::Candidates candidates = {{"a", {&a, true}}, {"b", {&b, true}}, {"c", {&c, true}},
                                                 {"d", {&d, true}}, {"e", {&e, false}}};
    const auto steps = byId(DependencyResolver(candidates, Version(0, 1, 0)).plan());
    EXPECT_EQ(steps.at("a").state, ModuleState::Active);
    EXPECT_EQ(steps.at("b").state, ModuleState::MissingDependency);
    EXPECT_EQ(steps.at("c").state, ModuleState::MissingDependency);
    EXPECT_NE(steps.at("c").detail.find("not installed"), std::string::npos);
    EXPECT_EQ(steps.at("e").state, ModuleState::Disabled);
    EXPECT_EQ(steps.at("d").state, ModuleState::MissingDependency);
    EXPECT_NE(steps.at("d").detail.find("disabled"), std::string::npos);
}

TEST(DependencyResolver, CyclesFailInsteadOfLooping) {
    const Manifest a = manifestOf("a", "1.0.0", {{"b", "*"}});
    const Manifest b = manifestOf("b", "1.0.0", {{"a", "*"}});
    DependencyResolver::Candidates candidates = {{"a", {&a, true}}, {"b", {&b, true}}};
    const auto steps = DependencyResolver(candidates, Version(0, 1, 0)).plan();
    ASSERT_EQ(steps.size(), 2u);
    for (const auto& s : steps) EXPECT_NE(s.state, ModuleState::Active) << s.id;
}

TEST(DependencyResolver, MutualOptionalDependenciesBothLoad) {
    Manifest a = manifestOf("a", "1.0.0");
    Manifest b = manifestOf("b", "1.0.0");
    a.optionalDependencies.emplace_back("b", VersionRange::parse("*"));
    b.optionalDependencies.emplace_back("a", VersionRange::parse("*"));
    DependencyResolver::Candidates candidates = {{"a", {&a, true}}, {"b", {&b, true}}};
    const auto steps = byId(DependencyResolver(candidates, Version(0, 1, 0)).plan());
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps.at("a").state, ModuleState::Active) << steps.at("a").detail;
    EXPECT_EQ(steps.at("b").state, ModuleState::Active) << steps.at("b").detail;
}

TEST(DependencyResolver, OptionalDependencyThatRequiresTheModuleLoadsAfterIt) {
    // a optionally uses b, b requires a: both load, a first, whichever is visited first.
    for (const auto& [first, second] : {std::pair{"a", "b"}, std::pair{"b", "a"}}) {
        Manifest user = manifestOf(first, "1.0.0");
        Manifest dependant = manifestOf(second, "1.0.0", {{first, "*"}});
        user.optionalDependencies.emplace_back(second, VersionRange::parse("*"));
        DependencyResolver::Candidates candidates = {{first, {&user, true}}, {second, {&dependant, true}}};
        const auto steps = DependencyResolver(candidates, Version(0, 1, 0)).plan();
        ASSERT_EQ(steps.size(), 2u);
        EXPECT_EQ(steps[0].id, first) << steps[0].detail;
        EXPECT_EQ(steps[0].state, ModuleState::Active) << steps[0].detail;
        EXPECT_EQ(steps[1].id, second) << steps[1].detail;
        EXPECT_EQ(steps[1].state, ModuleState::Active) << steps[1].detail;
    }
}

TEST(DependencyResolver, OptionalDependencyThatRequiresTheModuleThroughAnotherLoadsAfterIt) {
    // a optionally uses c, c requires b, b requires a: a, b, c.
    Manifest a = manifestOf("a", "1.0.0");
    const Manifest b = manifestOf("b", "1.0.0", {{"a", "*"}});
    const Manifest c = manifestOf("c", "1.0.0", {{"b", "*"}});
    a.optionalDependencies.emplace_back("c", VersionRange::parse("*"));
    DependencyResolver::Candidates candidates = {{"a", {&a, true}}, {"b", {&b, true}}, {"c", {&c, true}}};
    const auto steps = DependencyResolver(candidates, Version(0, 1, 0)).plan();
    ASSERT_EQ(steps.size(), 3u);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        EXPECT_EQ(steps[i].id, std::string(1, static_cast<char>('a' + i))) << steps[i].detail;
        EXPECT_EQ(steps[i].state, ModuleState::Active) << steps[i].detail;
    }
}

TEST(DependencyResolver, RemovedInThisHostVersion) {
    Manifest a = manifestOf("a", "1.0.0");
    a.deprecation = Deprecation{Version(0, 1, 0), Version(0, 4, 0), "", "gone"};
    DependencyResolver::Candidates candidates = {{"a", {&a, true}}};
    EXPECT_EQ(DependencyResolver(candidates, Version(0, 3, 0)).plan().front().state, ModuleState::Active);
    EXPECT_EQ(DependencyResolver(candidates, Version(0, 4, 0)).plan().front().state, ModuleState::Failed);
}
