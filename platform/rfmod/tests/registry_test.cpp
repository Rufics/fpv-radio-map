#include "rfmod/registry.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>

using namespace rfmod;
using namespace rfmod::test;
namespace fs = std::filesystem;

namespace {

class RecordingHost final : public IHostServices {
public:
    json call(const std::string& moduleId, const std::string& method, const json& request) override {
        ++calls;
        if (method == "test.ping") return {{"pong", request}, {"from", moduleId}};
        throw std::runtime_error("no such host method " + method);
    }
    void log(const std::string&, int, const std::string& message) override { lastLog = message; }
    std::atomic<int> calls{0};
    std::string lastLog;
};

class RegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / ("rfmod-registry-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                            "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        trust = std::make_unique<TrustStore>(ownerPublicText(), root / "keys.json");
        trust->add(thirdPartySecret().publicKey(), "Test vendor");
        config.root = root / "modules";
        config.workerExecutable = RFMOD_WORKER;
        config.hostVersion = Version(0, 1, 0);
        registry = std::make_unique<ModuleRegistry>(config, *trust, host);
    }
    void TearDown() override {
        registry.reset();
        fs::remove_all(root);
    }

    std::string pack(const json& manifest, const char* lib, const SecretKey& key) {
        return PackageWriter::pack(packageWith(manifest, lib), key);
    }

    static std::string failed(const ValidationReport& r) {
        for (const auto& c : r.checks()) {
            if (!c.ok) return c.name + ": " + c.detail;
        }
        return "";
    }

    fs::path root;
    RegistryConfig config;
    RecordingHost host;
    std::unique_ptr<TrustStore> trust;
    std::unique_ptr<ModuleRegistry> registry;
};

}  // namespace

TEST_F(RegistryTest, FirstPartyInstallsAndRunsInProcess) {
    const auto r = registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()), true);
    ASSERT_TRUE(r.ok()) << r.toJson().dump(2);
    IModuleInstance* m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    EXPECT_FALSE(m->isolated());
    EXPECT_EQ(m->invoke("echo", {{"a", 1}}), (json{{"a", 1}}));
    EXPECT_EQ(m->invoke("ping-host", {{"x", 2}}).at("from"), "test.alpha");
    EXPECT_EQ(registry->list().at(0).at("state"), "active");
    EXPECT_EQ(registry->withCapability("loss.contributor").size(), 1u);
    EXPECT_TRUE(registry->withCapability("route.analysis").empty());
}

TEST_F(RegistryTest, ThirdPartyRunsIsolatedAndSurvivesCrashes) {
    const auto r = registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret()), true);
    ASSERT_TRUE(r.ok()) << r.toJson().dump(2);
    IModuleInstance* m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->isolated());
    EXPECT_EQ(m->invoke("echo", {{"a", 1}}), (json{{"a", 1}}));
    // Host callbacks work across the process boundary.
    EXPECT_EQ(m->invoke("ping-host", {{"x", 2}}).at("pong"), (json{{"x", 2}}));
    // Errors keep their code.
    try {
        (void)m->invoke("bad", json::object());
        FAIL();
    } catch (const ModuleError& e) {
        EXPECT_EQ(e.code(), "bad_request");
    }
    // A crash kills only the worker; the host lives and the next call restarts it.
    try {
        (void)m->invoke("crash", json::object());
        FAIL();
    } catch (const ModuleError& e) {
        EXPECT_EQ(e.code(), "worker_crashed");
    }
    EXPECT_EQ(m->invoke("echo", {{"b", 2}}), (json{{"b", 2}}));
}

TEST_F(RegistryTest, IsolatedTimeoutAndMemoryLimit) {
    auto manifest = baseManifest("test.alpha", "1.0.0");
    manifest["limits"] = {{"memoryMb", 128}, {"timeoutMs", 300}};
    ASSERT_TRUE(registry->upload(pack(manifest, RFMOD_LIB_ALPHA, thirdPartySecret()), true).ok());
    IModuleInstance* m = registry->instance("test.alpha");
    try {
        (void)m->invoke("sleep", {{"ms", 5000}});
        FAIL();
    } catch (const ModuleError& e) {
        EXPECT_EQ(e.code(), "timeout");
    }
    try {
        (void)m->invoke("allocate", {{"mb", 1024}});
        FAIL();
    } catch (const ModuleError& e) {
        // std::bad_alloc inside the module becomes an internal error, or the worker dies.
        EXPECT_TRUE(e.code() == "internal" || e.code() == "worker_crashed") << e.code();
    }
    EXPECT_EQ(m->invoke("allocate", {{"mb", 16}}).at("bytes"), 16 << 20);
}

TEST_F(RegistryTest, FailingSelfTestIsNotInstalled) {
    const auto r = registry->upload(pack(baseManifest("test.broken", "1.0.0"), RFMOD_LIB_BROKEN, ownerSecret()), true);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 8), "selftest");
    EXPECT_EQ(registry->instance("test.broken"), nullptr);
    EXPECT_TRUE(registry->list().empty());
}

TEST_F(RegistryTest, LibraryMustMatchManifest) {
    // manifest.json says test.beta, the library says test.alpha.
    const auto r = registry->upload(pack(baseManifest("test.beta", "0.2.0"), RFMOD_LIB_ALPHA, ownerSecret()), true);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(failed(r).find("does not match"), std::string::npos) << failed(r);
}

TEST_F(RegistryTest, DependenciesAndUpgrade) {
    auto beta = baseManifest("test.beta", "0.2.0");
    beta["requires"] = {{{"id", "test.alpha"}, {"version", "^1.0.0"}}};
    // Order matters: the dependency must be installed first.
    auto r = registry->upload(pack(beta, RFMOD_LIB_BETA, ownerSecret()), true);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 12), "dependencies");

    ASSERT_TRUE(registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()), true).ok());
    ASSERT_TRUE(registry->upload(pack(beta, RFMOD_LIB_BETA, ownerSecret()), true).ok());
    EXPECT_NE(registry->instance("test.beta"), nullptr);

    // Cannot remove or disable what another module needs.
    EXPECT_THROW(registry->remove("test.alpha"), std::invalid_argument);
    EXPECT_THROW(registry->setEnabled("test.alpha", false), std::invalid_argument);

    // Upgrade in range keeps the dependent active; a downgrade is refused.
    ASSERT_TRUE(registry->upload(pack(baseManifest("test.alpha", "1.1.0"), RFMOD_LIB_ALPHA2, ownerSecret()), true).ok());
    EXPECT_EQ(registry->instance("test.beta") != nullptr, true);
    r = registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()), true);
    EXPECT_EQ(failed(r).substr(0, 15), "version.upgrade");

    // A major upgrade that leaves test.beta out of range is refused (lib content does not matter:
    // the check runs before the self-test).
    r = registry->upload(pack(baseManifest("test.alpha", "2.0.0"), RFMOD_LIB_ALPHA2, ownerSecret()), true);
    EXPECT_EQ(failed(r).substr(0, 10), "dependents");

    // Disabling the dependent first, then the dependency, works.
    registry->setEnabled("test.beta", false);
    registry->setEnabled("test.alpha", false);
    EXPECT_EQ(registry->instance("test.alpha"), nullptr);
    registry->setEnabled("test.alpha", true);
    registry->setEnabled("test.beta", true);
    EXPECT_NE(registry->instance("test.beta"), nullptr);
}

TEST_F(RegistryTest, DeprecationNotices) {
    auto alpha = baseManifest("test.alpha", "1.0.0");
    alpha["lifecycle"] = "deprecated";
    alpha["deprecation"] = {{"since", "0.1.0"}, {"removedIn", "0.4.0"}, {"replacement", "test.gamma"},
                            {"message", "Alpha is replaced by gamma."}};
    ASSERT_TRUE(registry->upload(pack(alpha, RFMOD_LIB_ALPHA, ownerSecret()), true).ok());
    auto beta = baseManifest("test.beta", "0.2.0");
    beta["requires"] = {{{"id", "test.alpha"}, {"version", "^1.0.0"}}};
    ASSERT_TRUE(registry->upload(pack(beta, RFMOD_LIB_BETA, ownerSecret()), true).ok());
    const auto notices = registry->notices();
    ASSERT_EQ(notices.size(), 2u);
    EXPECT_NE(notices[0].find("will be removed in 0.4.0"), std::string::npos);
    EXPECT_NE(notices[1].find("depends on deprecated test.alpha"), std::string::npos);

    // A host at the removal version no longer loads it, and the dependent reports why.
    registry.reset();
    config.hostVersion = Version(0, 4, 0);
    registry = std::make_unique<ModuleRegistry>(config, *trust, host);
    registry->reload();
    EXPECT_EQ(registry->instance("test.alpha"), nullptr);
    const json list = registry->list();
    EXPECT_EQ(list.at(0).at("state"), "failed");
    EXPECT_EQ(list.at(1).at("state"), "missing-dependency");
}

TEST_F(RegistryTest, RemovedKeyDisablesItsModulesOnReload) {
    ASSERT_TRUE(registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret()), true).ok());
    trust->remove(thirdPartySecret().publicKey().id());
    registry->reload();
    EXPECT_EQ(registry->instance("test.alpha"), nullptr);
    ASSERT_FALSE(registry->notices().empty());
    EXPECT_NE(registry->notices().front().find("does not trust"), std::string::npos);
}

TEST_F(RegistryTest, TamperedFilesOnDiskAreRestored) {
    ASSERT_TRUE(registry->upload(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()), true).ok());
    registry.reset();
    {
        std::ofstream out(config.root / "installed" / "test.alpha" / "lib" / "libmod.so", std::ios::app);
        out << "junk";
    }
    registry = std::make_unique<ModuleRegistry>(config, *trust, host);
    registry->reload();
    ASSERT_NE(registry->instance("test.alpha"), nullptr);
    EXPECT_NE(registry->notices().front().find("restored"), std::string::npos);
}

TEST_F(RegistryTest, OptionsAreValidated) {
    auto m = baseManifest("test.alpha", "1.0.0");
    m["optionsSchema"] = "schema/options.json";
    PackageFiles files = packageWith(m, RFMOD_LIB_ALPHA);
    files.files["schema/options.json"] =
        R"({"type":"object","additionalProperties":false,"properties":{"failInit":{"type":"boolean"}}})";
    ASSERT_TRUE(registry->upload(PackageWriter::pack(files, ownerSecret()), true).ok());
    EXPECT_THROW(registry->setOptions("test.alpha", {{"nope", 1}}), std::invalid_argument);
    registry->setOptions("test.alpha", {{"failInit", true}});
    EXPECT_EQ(registry->list().at(0).at("state"), "failed");
    registry->setOptions("test.alpha", {{"failInit", false}});
    EXPECT_EQ(registry->list().at(0).at("state"), "active");
    EXPECT_TRUE(registry->publicFile("test.alpha", "schema/options.json"));
    EXPECT_FALSE(registry->publicFile("test.alpha", "lib/libmod.so"));
}
