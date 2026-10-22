#include "rfmod/cli.hpp"
#include "rfmod/installer.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace rfmod;
using namespace rfmod::test;
namespace fs = std::filesystem;

namespace {

/// The rfmod tool run in-process against a scratch directory, with its output captured.
class CliTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = scratchDir("cli") / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    /// Runs the command line and returns its exit status; `out` and `err` hold what it printed.
    int run(std::vector<std::string> words, const std::string& ownerKey = ownerPublicText()) {
        out.str("");
        err.str("");
        const Cli cli({"0.1.0", ownerKey, RFMOD_WORKER}, out, err);
        return cli.run(words);
    }

    static void write(const fs::path& p, const std::string& content) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
    }

    /// A module project directory `rfmod pack` can read: the manifest and the test library.
    fs::path project(const std::string& id, const std::string& version, const char* lib, const json& extra = json::object()) {
        json m = baseManifest(id, version);
        m.update(extra);
        const fs::path p = dir / (id + "-src");
        write(p / "manifest.json", m.dump(2));
        write(p / "lib/libmod.so", readFile(lib));
        return p;
    }

    /// A signed package file of `id`, ready to check, test or install.
    fs::path package(const std::string& id, const std::string& version, const char* lib, const json& extra = json::object()) {
        const fs::path file = dir / (id + ".rfmod");
        write(file, PackageWriter::pack(packageWith([&] {
                                            json m = baseManifest(id, version);
                                            m.update(extra);
                                            return m;
                                        }(),
                                                    lib),
                                        ownerSecret()));
        return file;
    }

    [[nodiscard]] std::string ownerPub() const { return RFMOD_TEST_KEYS "/dev-owner.pub"; }
    [[nodiscard]] std::string ownerSecretFile() const { return RFMOD_TEST_KEYS "/dev-owner.secret"; }
    [[nodiscard]] std::string thirdPartyPub() const { return RFMOD_TEST_KEYS "/dev-thirdparty.pub"; }
    [[nodiscard]] std::string root() const { return (dir / "modules").string(); }
    [[nodiscard]] std::string keys() const { return (dir / "keys.json").string(); }

    fs::path dir;
    std::ostringstream out;
    std::ostringstream err;
};

}  // namespace

TEST_F(CliTest, UsageForNoCommandAndUnknownCommand) {
    EXPECT_EQ(run({}), 64);
    EXPECT_NE(err.str().find("usage: rfmod"), std::string::npos);
    EXPECT_EQ(run({"frobnicate"}), 64);
    EXPECT_NE(err.str().find("usage: rfmod"), std::string::npos);
}

TEST_F(CliTest, ArgumentErrorsArePrintedWithTheCommand) {
    EXPECT_EQ(run({"keygen"}), 1);
    EXPECT_EQ(err.str(), "rfmod keygen: rfmod keygen --out NAME\n");
    EXPECT_EQ(run({"keygen", "--out"}), 1);
    EXPECT_NE(err.str().find("missing value for --out"), std::string::npos);
    EXPECT_EQ(run({"check", "a.rfmod", "b.rfmod", "--trust", ownerPub()}), 1);
    EXPECT_NE(err.str().find("rfmod check FILE.rfmod"), std::string::npos);
}

TEST_F(CliTest, KeygenWritesAPairAndNeverOverwritesIt) {
    const std::string name = (dir / "me").string();
    ASSERT_EQ(run({"keygen", "--out", name}), 0) << err.str();
    EXPECT_TRUE(fs::exists(name + ".pub"));
    EXPECT_TRUE(fs::exists(name + ".secret"));
    EXPECT_EQ(fs::status(name + ".secret").permissions() & fs::perms::others_read, fs::perms::none);
    const PublicKey pub = PublicKey::fromText(readFile(name + ".pub"));
    EXPECT_NE(out.str().find("Key id " + pub.id()), std::string::npos);
    EXPECT_EQ(run({"keygen", "--out", name}), 1);
    EXPECT_NE(err.str().find("refusing to overwrite"), std::string::npos);
}

TEST_F(CliTest, NewScaffoldsAProjectOnce) {
    const std::string project = (dir / "acme").string();
    ASSERT_EQ(run({"new", "acme.rain", "--dir", project}), 0) << err.str();
    EXPECT_TRUE(fs::exists(project + "/manifest.json"));
    EXPECT_TRUE(fs::exists(project + "/src/module.cpp"));
    EXPECT_TRUE(fs::exists(project + "/CMakeLists.txt"));
    EXPECT_TRUE(fs::exists(project + "/schema/options.json"));
    EXPECT_TRUE(fs::exists(project + "/README.md"));
    const json manifest = json::parse(readFile(project + "/manifest.json"));
    EXPECT_EQ(manifest.at("id"), "acme.rain");
    EXPECT_EQ(manifest.at("entry").at("linux-x86_64"), "lib/libAcmeRainModule.so");
    EXPECT_NE(readFile(project + "/src/module.cpp").find("class AcmeRainModule final"), std::string::npos);
    EXPECT_EQ(run({"new", "acme.rain", "--dir", project}), 1);
    EXPECT_NE(err.str().find("already exists"), std::string::npos);
    EXPECT_EQ(run({"new"}), 1);
}

TEST_F(CliTest, PackInspectAndCheckAPackage) {
    const fs::path src = project("test.alpha", "1.0.0", RFMOD_LIB_ALPHA, {{"host", ">=0.1.0 <1.0.0"}});
    const std::string file = (dir / "alpha.rfmod").string();
    ASSERT_EQ(run({"pack", src.string(), "--key", ownerSecretFile(), "-o", file}), 0) << err.str();
    EXPECT_NE(out.str().find("Wrote " + file), std::string::npos);
    ASSERT_TRUE(fs::exists(file));

    ASSERT_EQ(run({"inspect", file}), 0) << err.str();
    EXPECT_NE(out.str().find("manifest.json"), std::string::npos);
    EXPECT_NE(out.str().find("\"id\": \"test.alpha\""), std::string::npos);

    ASSERT_EQ(run({"check", file, "--trust", ownerPub()}), 0) << out.str();
    EXPECT_NE(out.str().find("All checks passed."), std::string::npos);
    // A second --trust key is a third party; the package is still the owner's.
    EXPECT_EQ(run({"check", file, "--trust", ownerPub(), "--trust", thirdPartyPub()}), 0) << out.str();
    // Trusting only the third party refuses the owner's signature.
    EXPECT_EQ(run({"check", file, "--trust", thirdPartyPub()}), 1);
    EXPECT_NE(out.str().find("Package refused."), std::string::npos);
    // Without any key there is nothing to check against.
    EXPECT_EQ(run({"check", file}), 1);
    EXPECT_NE(err.str().find("--trust KEY.pub"), std::string::npos);
    // A host the manifest does not allow fails the host check.
    EXPECT_EQ(run({"check", file, "--trust", ownerPub(), "--host-version", "9.0.0"}), 1);
}

TEST_F(CliTest, PackNeedsAManifestAndAKey) {
    EXPECT_EQ(run({"pack", (dir / "nowhere").string(), "--key", ownerSecretFile()}), 1);
    EXPECT_NE(err.str().find("cannot read"), std::string::npos);
    const fs::path src = project("test.alpha", "1.0.0", RFMOD_LIB_ALPHA);
    EXPECT_EQ(run({"pack", src.string()}), 1);
    EXPECT_NE(err.str().find("--key NAME.secret"), std::string::npos);
}

TEST_F(CliTest, InspectReportsABrokenArchive) {
    const fs::path file = dir / "junk.rfmod";
    write(file, "not an archive");
    EXPECT_EQ(run({"inspect", file.string()}), 1);
    EXPECT_NE(out.str().find("Package refused."), std::string::npos);
}

TEST_F(CliTest, TestRunsTheSelfTestWithDependenciesAndFixtures) {
    const fs::path alpha = package("test.alpha", "1.0.0", RFMOD_LIB_ALPHA);
    const fs::path beta = package("test.beta", "0.2.0", RFMOD_LIB_BETA,
                                  {{"requires", {{{"id", "test.alpha"}, {"version", "^1.0.0"}}}}});
    const fs::path fixtures = dir / "fixtures.json";
    write(fixtures, json{{"test.ping", {{"pong", true}}}}.dump());
    ASSERT_EQ(run({"test", beta.string(), "--trust", ownerPub(), "--with", alpha.string(), "--fixtures", fixtures.string()}),
              0)
        << out.str() << err.str();
    EXPECT_NE(out.str().find("All checks passed."), std::string::npos);
    // Without the dependency the self-test is never reached.
    EXPECT_EQ(run({"test", beta.string(), "--trust", ownerPub()}), 1);
    EXPECT_NE(out.str().find("Package refused."), std::string::npos);
    // A dependency that is refused stops the test before the package itself.
    const fs::path broken = package("test.broken", "1.0.0", RFMOD_LIB_BROKEN);
    EXPECT_EQ(run({"test", beta.string(), "--trust", ownerPub(), "--with", broken.string()}), 1);
    EXPECT_NE(out.str().find("Dependency " + broken.string() + " was refused:"), std::string::npos);
    EXPECT_TRUE(fs::is_empty(fs::current_path()) ||
                std::ranges::none_of(fs::directory_iterator(fs::current_path()), [](const fs::directory_entry& e) {
                    return e.path().filename().string().starts_with(".rfmod-test-");
                }))
        << "the private test directory is removed";
}

TEST_F(CliTest, InstallListAndRemoveAsTheOperator) {
    const fs::path alpha = package("test.alpha", "1.0.0", RFMOD_LIB_ALPHA);
    // The operator commands need the server's trust store.
    EXPECT_EQ(run({"install", alpha.string(), "--root", root()}), 1);
    EXPECT_NE(err.str().find("--trust-store KEYS.json"), std::string::npos);
    // And a build with an owner key.
    EXPECT_EQ(run({"install", alpha.string(), "--root", root(), "--trust-store", keys()}, ""), 1);
    EXPECT_NE(err.str().find("without an owner key"), std::string::npos);

    ASSERT_EQ(run({"install", alpha.string(), "--root", root(), "--trust-store", keys()}), 0) << out.str() << err.str();
    EXPECT_NE(out.str().find("POST /api/v1/admin/modules/reload"), std::string::npos);
    ASSERT_EQ(run({"list", "--root", root(), "--trust-store", keys()}), 0) << err.str();
    EXPECT_NE(out.str().find("test.alpha 1.0.0  signed by"), std::string::npos);

    // A module another one requires stays until the dependent is gone.
    const fs::path beta = package("test.beta", "0.2.0", RFMOD_LIB_BETA,
                                  {{"requires", {{{"id", "test.alpha"}, {"version", "^1.0.0"}}}}});
    ASSERT_EQ(run({"install", beta.string(), "--root", root(), "--trust-store", keys()}), 0) << out.str() << err.str();
    EXPECT_EQ(run({"remove", "test.alpha", "--root", root(), "--trust-store", keys()}), 1);
    EXPECT_NE(err.str().find("is required by test.beta"), std::string::npos);
    ASSERT_EQ(run({"remove", "test.beta", "--root", root(), "--trust-store", keys()}), 0) << err.str();
    ASSERT_EQ(run({"remove", "test.alpha", "--root", root(), "--trust-store", keys()}), 0) << err.str();
    EXPECT_NE(out.str().find("Removed test.alpha."), std::string::npos);
    ASSERT_EQ(run({"list", "--root", root(), "--trust-store", keys()}), 0);
    EXPECT_EQ(out.str(), "");
    EXPECT_EQ(run({"remove", "test.alpha", "--root", root(), "--trust-store", keys()}), 1);
    EXPECT_NE(err.str().find("unknown module"), std::string::npos);
    EXPECT_EQ(run({"remove", "..", "--root", root(), "--trust-store", keys()}), 1);
    EXPECT_NE(err.str().find("unknown module"), std::string::npos) << err.str();
    EXPECT_TRUE(fs::exists(fs::path(root()) / "installed"));
}

TEST_F(CliTest, InstallRefusesAFailingSelfTest) {
    const fs::path broken = package("test.broken", "1.0.0", RFMOD_LIB_BROKEN);
    EXPECT_EQ(run({"install", broken.string(), "--root", root(), "--trust-store", keys()}), 1);
    EXPECT_NE(out.str().find("Package refused."), std::string::npos);
    EXPECT_FALSE(fs::exists(fs::path(root()) / "installed" / "test.broken"));
}

TEST_F(CliTest, TrustAddsListsAndRemovesKeys) {
    ASSERT_EQ(run({"trust", "list", "--trust-store", keys()}), 0) << err.str();
    const std::string owner = PublicKey::fromText(ownerPublicText()).id();
    EXPECT_NE(out.str().find(owner + "  "), std::string::npos);
    EXPECT_NE(out.str().find("(built in)"), std::string::npos);

    ASSERT_EQ(run({"trust", "add", thirdPartyPub(), "--label", "Acme", "--trust-store", keys()}), 0) << err.str();
    const std::string third = thirdPartySecret().publicKey().id();
    EXPECT_EQ(out.str(), "Trusted " + third + " (Acme)\n");
    ASSERT_EQ(run({"trust", "list", "--trust-store", keys()}), 0);
    EXPECT_NE(out.str().find(third + "  Acme\n"), std::string::npos);

    ASSERT_EQ(run({"trust", "remove", third, "--trust-store", keys()}), 0) << err.str();
    ASSERT_EQ(run({"trust", "list", "--trust-store", keys()}), 0);
    EXPECT_EQ(out.str().find(third), std::string::npos);

    EXPECT_EQ(run({"trust", "--trust-store", keys()}), 1);
    EXPECT_EQ(run({"trust", "add", thirdPartyPub(), "--trust-store", keys()}), 1);
    EXPECT_EQ(run({"trust", "frob", "--trust-store", keys()}), 1);
    EXPECT_NE(err.str().find("rfmod trust list|add"), std::string::npos);
}
