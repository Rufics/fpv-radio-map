#include "test_support.hpp"

#include <gtest/gtest.h>

using namespace rfmod;
using namespace rfmod::test;

namespace {

bool parses(const nlohmann::json& m, std::string* failed = nullptr, Version host = Version(0, 1, 0)) {
    ValidationReport r;
    const bool ok = ManifestParser::parse(m, host, r).has_value();
    if (failed) {
        for (const auto& c : r.checks()) {
            if (!c.ok) {
                *failed = c.name;
                break;
            }
        }
    }
    return ok;
}

}  // namespace

TEST(Manifest, MinimalIsValid) { EXPECT_TRUE(parses(baseManifest("fpv.battery", "1.0.0"))); }

TEST(Manifest, SchemaErrors) {
    std::string failed;
    auto m = baseManifest("Bad Id", "1.0.0");
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "manifest.schema");
    m = baseManifest("fpv.battery", "1.0.0");
    m["unknown"] = 1;
    EXPECT_FALSE(parses(m, &failed));
    m = baseManifest("fpv.battery", "1.0.0");
    m["abi"] = 2;
    EXPECT_FALSE(parses(m, &failed));
    m = baseManifest("fpv.battery", "1.0.0");
    m["lifecycle"] = "deprecated";  // deprecated needs a deprecation block
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "manifest.schema");
}

TEST(Manifest, HostRangeAndPlatform) {
    std::string failed;
    auto m = baseManifest("fpv.battery", "1.0.0");
    m["host"] = ">=2.0.0";
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "host.version");
    m = baseManifest("fpv.battery", "1.0.0");
    m["entry"] = {{"plan9-mips", "lib/x.so"}};
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "entry.platform");
}

TEST(Manifest, DeprecationWindow) {
    std::string failed;
    auto m = baseManifest("fpv.oldthing", "1.4.0");
    m["lifecycle"] = "deprecated";
    m["deprecation"] = {{"since", "0.4.0"}, {"removedIn", "0.6.0"}, {"message", "Use fpv.newthing instead."}};
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "deprecation.window");
    m["deprecation"]["removedIn"] = "0.7.0";
    EXPECT_TRUE(parses(m));
    m["deprecation"]["removedIn"] = "1.0.0";  // a later major is always far enough
    EXPECT_TRUE(parses(m));
}

TEST(Manifest, DependenciesMustBeDistinct) {
    std::string failed;
    auto m = baseManifest("fpv.relay", "1.0.0");
    m["requires"] = {{{"id", "fpv.core"}, {"version", ">=1.0.0"}}, {{"id", "fpv.core"}, {"version", "<2.0.0"}}};
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "manifest.dependencies");
    m["requires"] = {{{"id", "fpv.core"}, {"version", ">=1.0 <2"}}};
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "manifest.versions");
}

TEST(Manifest, UiNeedsCapability) {
    std::string failed;
    auto m = baseManifest("fpv.battery", "1.0.0");
    m["ui"] = {{"bundle", "ui/battery.js"}, {"slots", {"side.panels"}}};
    EXPECT_FALSE(parses(m, &failed));
    EXPECT_EQ(failed, "ui.capability");
    m["provides"].push_back("ui.panel");
    EXPECT_TRUE(parses(m));
}
