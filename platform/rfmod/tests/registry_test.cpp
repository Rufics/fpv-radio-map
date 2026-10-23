#include "rfmod/installer.hpp"
#include "rfmod/module_store.hpp"
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

/// A module root with an installer (the CLI's side) and a registry (the server's side) on it.
class RegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = scratchDir("registry") / ("rfmod-registry-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        trust = std::make_unique<TrustStore>(ownerPublicText(), root / "keys.json");
        trust->add(thirdPartySecret().publicKey(), "Test vendor");
        config.root = root / "modules";
        config.workerExecutable = RFMOD_WORKER;
        config.hostVersion = Version(0, 1, 0);
        installerConfig.root = config.root;
        installerConfig.workerExecutable = RFMOD_WORKER;
        installerConfig.hostVersion = config.hostVersion;
        installer = std::make_unique<ModuleInstaller>(installerConfig, *trust, host);
        registry = std::make_unique<ModuleRegistry>(config, *trust, host);
    }
    void TearDown() override {
        registry.reset();
        installer.reset();
        fs::remove_all(root);
    }

    static std::string pack(const json& manifest, const char* lib, const SecretKey& key) {
        return PackageWriter::pack(packageWith(manifest, lib), key);
    }

    /// Installs as the CLI would and reloads as the server would after POST /admin/modules/reload.
    ValidationReport install(const std::string& archive) {
        const ValidationReport r = installer->install(archive);
        registry->reload();
        return r;
    }

    static std::string failed(const ValidationReport& r) {
        for (const auto& c : r.checks()) {
            if (!c.ok) return c.name + ": " + c.detail;
        }
        return "";
    }

    fs::path root;
    RegistryConfig config;
    InstallerConfig installerConfig;
    RecordingHost host;
    std::unique_ptr<TrustStore> trust;
    std::unique_ptr<ModuleInstaller> installer;
    std::unique_ptr<ModuleRegistry> registry;
};

}  // namespace

TEST_F(RegistryTest, FirstPartyInstallsAndRunsInProcess) {
    const auto r = install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()));
    ASSERT_TRUE(r.ok()) << r.toJson().dump(2);
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    EXPECT_FALSE(m->isolated());
    EXPECT_EQ(m->invoke("echo", {{"a", 1}}), (json{{"a", 1}}));
    EXPECT_EQ(m->invoke("ping-host", {{"x", 2}}).at("from"), "test.alpha");
    EXPECT_EQ(registry->list().at(0).at("state"), "active");
    EXPECT_EQ(registry->withCapability("loss.contributor").size(), 1u);
    EXPECT_TRUE(registry->withCapability("route.analysis").empty());
}

TEST_F(RegistryTest, FlightLogDecodersAlwaysRunIsolated) {
    json manifest = baseManifest("test.alpha", "1.0.0");
    manifest["provides"] = json::array({"flightlog.decoder"});
    const auto r = install(pack(manifest, RFMOD_LIB_ALPHA, ownerSecret()));
    ASSERT_TRUE(r.ok()) << r.toJson().dump(2);
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(m->isolated()) << "first party, but it parses uploaded files";
    EXPECT_TRUE(registry->list().at(0).at("isolated").get<bool>());
    EXPECT_EQ(registry->withCapability("flightlog.decoder").size(), 1u);
}

TEST_F(RegistryTest, ThirdPartyRunsIsolatedAndSurvivesCrashes) {
    const auto r = install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret()));
    ASSERT_TRUE(r.ok()) << r.toJson().dump(2);
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
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

TEST_F(RegistryTest, WorkerSandboxConfinesTheModule) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret())).ok());
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    const auto& iso = dynamic_cast<const IsolatedModule&>(*m);
    EXPECT_TRUE(iso.sandbox().complete()) << iso.sandbox().describe();
    const json probe = m->invoke("probe", json::object());
    EXPECT_TRUE(probe.at("readsOwnDir").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("readsEtc").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("opensSocket").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("forks").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("signalsOthers").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("truncatesOwnFile").get<bool>()) << probe.dump();
    EXPECT_FALSE(probe.at("writesHere").get<bool>()) << probe.dump();
}

TEST_F(RegistryTest, ALibraryThatDoesNotLoadReportsItsOwnError) {
    // A third-party module requires the sandbox; a failure after confinement is still the
    // module's failure, not reported as an incomplete sandbox.
    const fs::path junk = root / "not-a-library.so";
    fs::create_directories(root);
    std::ofstream(junk) << "this is not an ELF file";
    const auto r = install(pack(baseManifest("test.alpha", "1.0.0"), junk.c_str(), thirdPartySecret()));
    EXPECT_FALSE(r.ok());
    const std::string why = failed(r);
    EXPECT_NE(why.find("dlopen"), std::string::npos) << why;
    EXPECT_EQ(why.find("confine"), std::string::npos) << why;
}

TEST_F(RegistryTest, IsolatedTimeoutAndMemoryLimit) {
    auto manifest = baseManifest("test.alpha", "1.0.0");
    manifest["limits"] = {{"memoryMb", 128}, {"timeoutMs", 300}};
    ASSERT_TRUE(install(pack(manifest, RFMOD_LIB_ALPHA, thirdPartySecret())).ok());
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
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

TEST_F(RegistryTest, PolicyCapsManifestLimits) {
    auto manifest = baseManifest("test.alpha", "1.0.0");
    manifest["limits"] = {{"memoryMb", 4096}, {"timeoutMs", 600000}};
    config.policy.maxMemoryMb = 256;
    config.policy.maxTimeoutMs = 200;
    registry = std::make_unique<ModuleRegistry>(config, *trust, host);
    ASSERT_TRUE(install(pack(manifest, RFMOD_LIB_ALPHA, thirdPartySecret())).ok());
    const std::shared_ptr<IModuleInstance> m = registry->instance("test.alpha");
    ASSERT_NE(m, nullptr);
    const auto& iso = dynamic_cast<const IsolatedModule&>(*m);
    EXPECT_EQ(iso.limits().memoryMb, 256u);
    EXPECT_EQ(iso.limits().timeoutMs, 200u);
    try {
        (void)m->invoke("sleep", {{"ms", 2000}});
        FAIL();
    } catch (const ModuleError& e) {
        EXPECT_EQ(e.code(), "timeout");
    }
}

TEST_F(RegistryTest, InstancesOutliveAReloadSafely) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
    const std::shared_ptr<IModuleInstance> held = registry->instance("test.alpha");
    ASSERT_NE(held, nullptr);
    registry->reload();
    EXPECT_NE(registry->instance("test.alpha"), held);
    try {
        (void)held->invoke("echo", json::object());
        FAIL();
    } catch (const ModuleError& e) {
        EXPECT_EQ(e.code(), "unavailable");
    }
}

TEST_F(RegistryTest, FailingSelfTestIsNotInstalled) {
    const auto r = install(pack(baseManifest("test.broken", "1.0.0"), RFMOD_LIB_BROKEN, ownerSecret()));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 8), "selftest");
    EXPECT_EQ(registry->instance("test.broken"), nullptr);
    EXPECT_TRUE(registry->list().empty());
    EXPECT_TRUE(installer->installed().empty());
}

TEST_F(RegistryTest, CheckRunsEverythingWithoutInstalling) {
    const auto r = installer->check(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()));
    EXPECT_TRUE(r.ok()) << r.toJson().dump(2);
    EXPECT_TRUE(installer->installed().empty());
    EXPECT_TRUE(fs::is_empty(config.root / "staging"));
}

TEST_F(RegistryTest, LibraryMustMatchManifest) {
    // manifest.json says test.beta, the library says test.alpha.
    const auto r = install(pack(baseManifest("test.beta", "0.2.0"), RFMOD_LIB_ALPHA, ownerSecret()));
    EXPECT_FALSE(r.ok());
    EXPECT_NE(failed(r).find("does not match"), std::string::npos) << failed(r);
}

TEST_F(RegistryTest, DependenciesAndUpgrade) {
    auto beta = baseManifest("test.beta", "0.2.0");
    beta["requires"] = {{{"id", "test.alpha"}, {"version", "^1.0.0"}}};
    // Order matters: the dependency must be installed first.
    auto r = install(pack(beta, RFMOD_LIB_BETA, ownerSecret()));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 12), "dependencies");

    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
    ASSERT_TRUE(install(pack(beta, RFMOD_LIB_BETA, ownerSecret())).ok());
    EXPECT_NE(registry->instance("test.beta"), nullptr);

    // Cannot remove or disable what another module needs.
    EXPECT_THROW(installer->remove("test.alpha"), std::invalid_argument);
    EXPECT_THROW(registry->setEnabled("test.alpha", false), std::invalid_argument);

    // Upgrade in range keeps the dependent active; a downgrade is refused.
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.1.0"), RFMOD_LIB_ALPHA2, ownerSecret())).ok());
    EXPECT_NE(registry->instance("test.beta"), nullptr);
    r = install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret()));
    EXPECT_EQ(failed(r).substr(0, 15), "version.upgrade");

    // A major upgrade that leaves test.beta out of range is refused (lib content does not matter:
    // the check runs before the self-test).
    r = install(pack(baseManifest("test.alpha", "2.0.0"), RFMOD_LIB_ALPHA2, ownerSecret()));
    EXPECT_EQ(failed(r).substr(0, 10), "dependents");

    // Disabling the dependent first, then the dependency, works.
    registry->setEnabled("test.beta", false);
    registry->setEnabled("test.alpha", false);
    EXPECT_EQ(registry->instance("test.alpha"), nullptr);
    registry->setEnabled("test.alpha", true);
    registry->setEnabled("test.beta", true);
    EXPECT_NE(registry->instance("test.beta"), nullptr);

    // Removing in the right order works and the server notices at the next reload.
    installer->remove("test.beta");
    installer->remove("test.alpha");
    registry->reload();
    EXPECT_TRUE(registry->list().empty());
}

TEST_F(RegistryTest, AnotherKeyCannotTakeOverAModule) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret())).ok());
    auto other = std::make_unique<TrustStore>(ownerPublicText(), root / "keys.json");
    const SecretKey otherVendor = SecretKey::generate();
    other->add(otherVendor.publicKey(), "Other vendor");
    ModuleInstaller otherInstaller(installerConfig, *other, host);
    const auto r = otherInstaller.install(pack(baseManifest("test.alpha", "1.1.0"), RFMOD_LIB_ALPHA2, otherVendor));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 6), "signer") << failed(r);
    // The owner may always replace it.
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.1.0"), RFMOD_LIB_ALPHA2, ownerSecret())).ok());
}

TEST_F(RegistryTest, ReservedIdsNeedTheOwnerKey) {
    EXPECT_TRUE(ModuleInstaller::reservedId("fpv.core"));
    EXPECT_TRUE(ModuleInstaller::reservedId("rf.ext"));
    EXPECT_FALSE(ModuleInstaller::reservedId("test.alpha"));
    auto manifest = baseManifest("test.alpha", "1.0.0");
    manifest["id"] = "fpv.alpha";
    const auto r = installer->check(pack(manifest, RFMOD_LIB_ALPHA, thirdPartySecret()));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(failed(r).substr(0, 15), "signer.reserved") << failed(r);
}

TEST_F(RegistryTest, DeprecationNotices) {
    auto alpha = baseManifest("test.alpha", "1.0.0");
    alpha["lifecycle"] = "deprecated";
    alpha["deprecation"] = {{"since", "0.1.0"}, {"removedIn", "0.4.0"}, {"replacement", "test.gamma"},
                            {"message", "Alpha is replaced by gamma."}};
    ASSERT_TRUE(install(pack(alpha, RFMOD_LIB_ALPHA, ownerSecret())).ok());
    auto beta = baseManifest("test.beta", "0.2.0");
    beta["requires"] = {{{"id", "test.alpha"}, {"version", "^1.0.0"}}};
    ASSERT_TRUE(install(pack(beta, RFMOD_LIB_BETA, ownerSecret())).ok());
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
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, thirdPartySecret())).ok());
    // The CLI edits the store file; the server's TrustStore reads it again on reload.
    TrustStore cli(ownerPublicText(), root / "keys.json");
    cli.remove(thirdPartySecret().publicKey().id());
    trust->reload();
    registry->reload();
    EXPECT_EQ(registry->instance("test.alpha"), nullptr);
    ASSERT_FALSE(registry->notices().empty());
    EXPECT_NE(registry->notices().front().find("does not trust"), std::string::npos);
}

TEST_F(RegistryTest, AFailedReplaceKeepsTheInstalledModule) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
    const ModuleStore store(config.root);
    const fs::path installedDir = config.root / "installed" / "test.alpha";
    // The staged directory vanished before the swap: the rename throws after the old version was set aside.
    EXPECT_THROW(store.replace(config.root / "staging" / "gone", "test.alpha"), fs::filesystem_error);
    EXPECT_TRUE(fs::exists(installedDir / "manifest.json"));
    registry->reload();
    EXPECT_NE(registry->instance("test.alpha"), nullptr);
}

TEST_F(RegistryTest, TamperedFilesOnDiskAreRestored) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
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

TEST_F(RegistryTest, UnreadableStateFileFallsBackToDefaults) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
    {
        std::ofstream out(config.root / "installed" / "test.alpha" / "state.json", std::ios::trunc);
        out << "{not json";
    }
    registry->reload();
    EXPECT_NE(registry->instance("test.alpha"), nullptr);
    EXPECT_NE(registry->notices().front().find("state.json"), std::string::npos);
}

TEST_F(RegistryTest, AnInstallWithoutAReloadNeverServesTheNewVersionsFiles) {
    // The server serves what it loaded. `rfmod install` swaps the directory on disk in its own
    // process; until the admin reloads, the loaded version's files (or none) are served, never
    // the new version's bytes under the loaded version's lifecycle.
    auto v1 = baseManifest("test.alpha", "1.0.0");
    v1["provides"].push_back("ui.panel");
    v1["ui"] = {{"bundle", "ui/x.js"}, {"slots", {"side.panels"}}};
    PackageFiles one = packageWith(v1, RFMOD_LIB_ALPHA);
    one.files["ui/x.js"] = "export const version = 1;";
    ASSERT_TRUE(install(PackageWriter::pack(one, ownerSecret())).ok());
    const auto before = registry->publicFile("test.alpha", "ui/x.js");
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->content, "export const version = 1;");
    EXPECT_EQ(before->lifecycle, Lifecycle::Beta);

    auto v2 = v1;
    v2["version"] = "1.1.0";
    v2["lifecycle"] = "alpha";
    PackageFiles two = packageWith(v2, RFMOD_LIB_ALPHA2);
    two.files["ui/x.js"] = "export const version = 2;";
    const auto upgraded = installer->install(PackageWriter::pack(two, ownerSecret()));  // the CLI; no reload yet
    ASSERT_TRUE(upgraded.ok()) << upgraded.toJson().dump(2);
    if (const auto during = registry->publicFile("test.alpha", "ui/x.js")) {
        EXPECT_EQ(during->content, "export const version = 1;");
        EXPECT_EQ(during->lifecycle, Lifecycle::Beta);
    }
    EXPECT_FALSE(registry->publicFile("test.alpha", "ui/../lib/libmod.so"));

    registry->reload();
    const auto after = registry->publicFile("test.alpha", "ui/x.js");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->content, "export const version = 2;");
    EXPECT_EQ(after->lifecycle, Lifecycle::Alpha);
}

TEST_F(RegistryTest, AFileBehindASymlinkInsideTheModuleIsNotServed) {
    // O_NOFOLLOW guards only the last path part. A symlinked directory under ui/ must not lead
    // a public path to a file outside ui/ (here the module's own library) or outside the module.
    auto m = baseManifest("test.alpha", "1.0.0");
    m["provides"].push_back("ui.panel");
    m["ui"] = {{"bundle", "ui/x.js"}, {"slots", {"side.panels"}}};
    PackageFiles files = packageWith(m, RFMOD_LIB_ALPHA);
    files.files["ui/x.js"] = "export const version = 1;";
    ASSERT_TRUE(install(PackageWriter::pack(files, ownerSecret())).ok());
    const fs::path ui = config.root / "installed" / "test.alpha" / "ui";
    fs::create_directory_symlink("../lib", ui / "sub");
    fs::create_symlink("x.js", ui / "alias.js");
    ASSERT_TRUE(fs::exists(ui / "sub" / "libmod.so"));

    EXPECT_FALSE(registry->publicFile("test.alpha", "ui/sub/libmod.so"));
    EXPECT_FALSE(registry->publicFile("test.alpha", "ui/alias.js"));
    const auto direct = registry->publicFile("test.alpha", "ui/x.js");
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(direct->content, "export const version = 1;");
}

TEST_F(RegistryTest, OptionsAreValidated) {
    auto m = baseManifest("test.alpha", "1.0.0");
    m["optionsSchema"] = "schema/options.json";
    PackageFiles files = packageWith(m, RFMOD_LIB_ALPHA);
    files.files["schema/options.json"] =
        R"({"type":"object","additionalProperties":false,"properties":{"failInit":{"type":"boolean"}}})";
    ASSERT_TRUE(install(PackageWriter::pack(files, ownerSecret())).ok());
    EXPECT_THROW(registry->setOptions("test.alpha", {{"nope", 1}}), std::invalid_argument);
    EXPECT_TRUE(registry->checkOptions("test.alpha", {{"nope", 1}}).has_value());
    EXPECT_TRUE(registry->checkOptions("test.alpha", json::array()).has_value());
    EXPECT_FALSE(registry->checkOptions("test.alpha", {{"failInit", false}}).has_value());
    EXPECT_TRUE(registry->checkOptions("test.nobody", json::object()).has_value());
    registry->setOptions("test.alpha", {{"failInit", true}});
    EXPECT_EQ(registry->list().at(0).at("state"), "failed");
    registry->setOptions("test.alpha", {{"failInit", false}});
    EXPECT_EQ(registry->list().at(0).at("state"), "active");
    const auto schema = registry->publicFile("test.alpha", "schema/options.json");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->path.filename(), "options.json");
    EXPECT_NE(schema->content.find("failInit"), std::string::npos);  // read with the lifecycle, from the same version
    EXPECT_EQ(schema->lifecycle, Lifecycle::Beta);  // the file and its module's lifecycle come as one snapshot
    EXPECT_FALSE(registry->publicFile("test.alpha", "lib/libmod.so"));
    EXPECT_FALSE(registry->publicFile("test.alpha", "ui/../lib/libmod.so"));
    EXPECT_FALSE(registry->publicFile("test.alpha", "ui/../../other/ui/x.js"));
}

TEST_F(RegistryTest, ModulesWithoutSchemaTakeNoOptions) {
    ASSERT_TRUE(install(pack(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA, ownerSecret())).ok());
    EXPECT_FALSE(registry->checkOptions("test.alpha", json::object()).has_value());
    EXPECT_TRUE(registry->checkOptions("test.alpha", {{"anything", 1}}).has_value());
    EXPECT_THROW(registry->setOptions("test.alpha", {{"anything", 1}}), std::invalid_argument);
}
