// rfmod: the module developer and operator tool.
//
// Developer commands:
//   rfmod keygen --out NAME              write NAME.pub and NAME.secret (keep the secret offline)
//   rfmod new ID [--dir DIR]             scaffold a module project
//   rfmod pack DIR --key NAME.secret [-o FILE.rfmod]
//   rfmod inspect FILE.rfmod
//   rfmod check FILE.rfmod --trust KEY.pub [--trust ...] [--host-version X.Y.Z]
//   rfmod test FILE.rfmod --trust KEY.pub [--with DEP.rfmod ...] [--fixtures FILE.json]
//
// Operator commands, run on the server machine against its module root. They are the only way a
// module gets onto a server: the HTTP API lists modules and toggles them, it never installs.
//   rfmod install FILE.rfmod --root DIR --trust-store KEYS.json [--fixtures FILE.json]
//   rfmod remove ID --root DIR --trust-store KEYS.json
//   rfmod list --root DIR --trust-store KEYS.json
//   rfmod trust list|add KEY.pub --label TEXT|remove KEY_ID  --trust-store KEYS.json
//
// "check" runs every install check except the self-test; "test" and "install" also run the
// self-test in a worker exactly as the server would. Host calls are answered from --fixtures
// ({"method": response, ...}); anything else fails, so self-tests should not depend on them.
// The operator commands trust the owner key this build was made with plus the keys in the trust
// store file; the developer commands trust only the --trust keys, the first of which plays the
// owner's part.

#include "rfmod/installer.hpp"
#include "rfmod/package.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using rfmod::json;

namespace {

// The build passes the project version and owner key; a stand-alone build of this file falls back.
#ifdef RFMAP_HOST_VERSION
constexpr const char* kHostVersion = RFMAP_HOST_VERSION;
#else
constexpr const char* kHostVersion = "0.1.0";
#endif
#ifdef RFMAP_OWNER_PUBKEY_TEXT
constexpr const char* kOwnerKey = RFMAP_OWNER_PUBKEY_TEXT;
#else
constexpr const char* kOwnerKey = "";
#endif

/// A file the tool was asked to read or write could not be.
class FileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A host call a module made that --fixtures does not answer.
class MissingFixture : public std::runtime_error {
public:
    explicit MissingFixture(const std::string& method)
        : std::runtime_error(method + " is not available in rfmod (add it to --fixtures)") {}
};

/// The command line: positional words and --flag VALUE pairs.
class Args {
public:
    explicit Args(std::span<char* const> words) {
        auto next = words.begin();
        while (next != words.end()) {
            const std::string s = *next++;
            if (!s.starts_with("--") && s != "-o") {
                positional_.push_back(s);
                continue;
            }
            if (next == words.end()) throw std::invalid_argument("missing value for " + s);
            flags_.emplace(s == "-o" ? "--out" : s, *next++);
        }
    }

    [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }
    /// The one positional word a command takes; throws with `usage` otherwise.
    [[nodiscard]] const std::string& only(const char* usage) const {
        if (positional_.size() != 1) throw std::invalid_argument(usage);
        return positional_.front();
    }
    [[nodiscard]] std::string get(std::string_view k, const std::string& fallback = {}) const {
        const auto it = flags_.find(k);
        return it == flags_.end() ? fallback : it->second;
    }
    /// The flag's value; throws with `usage` when it is missing.
    [[nodiscard]] std::string need(std::string_view k, const char* usage) const {
        const std::string v = get(k);
        if (v.empty()) throw std::invalid_argument(usage);
        return v;
    }
    [[nodiscard]] std::vector<std::string> all(std::string_view k) const {
        std::vector<std::string> out;
        const auto [first, last] = flags_.equal_range(k);
        for (const std::string& value : std::ranges::subrange(first, last) | std::views::values) out.push_back(value);
        return out;
    }

private:
    std::vector<std::string> positional_;
    std::multimap<std::string, std::string, std::less<>> flags_;
};

/// Files the tool reads and writes, with the errors named after the file.
class Files {
public:
    [[nodiscard]] static std::string read(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) throw FileError("cannot read " + p.string());
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    static void write(const fs::path& p, const std::string& content, mode_t mode = 0644) {
        if (p.has_parent_path()) fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) throw FileError("cannot write " + p.string());
        out.close();
        chmod(p.c_str(), mode);
    }
};

/// Answers a self-test's host calls from a fixtures file and prints its log lines.
class FixtureHost final : public rfmod::IHostServices {
public:
    explicit FixtureHost(json fixtures) : fixtures_(std::move(fixtures)) {}
    json call(const std::string&, const std::string& method, const json&) override {
        if (fixtures_.contains(method)) return fixtures_.at(method);
        throw MissingFixture(method);
    }
    void log(const std::string& id, int level, const std::string& message) override {
        static constexpr std::array<const char*, 4> names = {"debug", "info", "warn", "error"};
        std::cerr << "[" << id << " " << names.at(static_cast<std::size_t>(std::clamp(level, 0, 3))) << "] " << message
                  << "\n";
    }

private:
    json fixtures_;
};

/// A new directory only this user can open (mkdtemp: mode 0700, unguessable name), removed with
/// everything in it when it goes out of scope.
class PrivateDir {
public:
    explicit PrivateDir(const fs::path& prefix) {
        std::string name = prefix.string() + "XXXXXX";
        if (!mkdtemp(name.data())) throw FileError("cannot create a directory next to " + prefix.string());
        path_ = name;
    }
    ~PrivateDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    PrivateDir(const PrivateDir&) = delete;
    PrivateDir& operator=(const PrivateDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/// The files `rfmod new` writes for a module project.
class Scaffold {
public:
    Scaffold(std::string id, fs::path dir) : id_(std::move(id)), dir_(std::move(dir)), cls_(className(id_)) {}

    void write() const {
        const json manifest = manifestFor();
        Files::write(dir_ / "manifest.json", manifest.dump(2) + "\n");
        Files::write(dir_ / "schema/options.json", optionsSchema().dump(2) + "\n");
        Files::write(dir_ / "CMakeLists.txt", cmake());
        Files::write(dir_ / "src/module.cpp", source(manifest));
        Files::write(dir_ / "README.md", readme());
    }

private:
    [[nodiscard]] static std::string className(const std::string& id) {
        std::string cls;
        bool up = true;
        for (const char c : id) {
            if (c == '.' || c == '-') {
                up = true;
                continue;
            }
            cls += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            up = false;
        }
        return cls + "Module";
    }

    [[nodiscard]] json manifestFor() const {
        return {{"schema", "rfmod/v1"},
                {"id", id_},
                {"version", "0.1.0"},
                {"title", "My module"},
                {"description", "What this module adds to the link budget."},
                {"author", ""},
                {"license", "MIT"},
                {"lifecycle", "alpha"},
                {"abi", RFMOD_ABI_VERSION},
                {"host", ">=0.1.0 <1.0.0"},
                {"requires", json::array()},
                {"provides", {"loss.contributor"}},
                {"methods", {"loss.contribute"}},
                {"entry", {{"linux-x86_64", "lib/lib" + cls_ + ".so"}}},
                {"optionsSchema", "schema/options.json"},
                {"limits", {{"memoryMb", 256}, {"timeoutMs", 5000}}}};
    }

    [[nodiscard]] static json optionsSchema() {
        return {{"$schema", "http://json-schema.org/draft-07/schema#"},
                {"type", "object"},
                {"additionalProperties", false},
                {"properties", {{"extraLossDb", {{"type", "number"}, {"minimum", 0}, {"default", 0}}}}}};
    }

    [[nodiscard]] std::string cmake() const {
        return "cmake_minimum_required(VERSION 3.20)\nproject(" + cls_ + " LANGUAGES CXX)\n"
               "set(CMAKE_CXX_STANDARD 20)\n"
               "find_package(nlohmann_json REQUIRED)\n"
               "# Point RFMOD_SDK at platform/rfmod/include of the rfmap source tree.\n"
               "set(RFMOD_SDK \"\" CACHE PATH \"rfmod SDK include directory\")\n"
               "add_library(" + cls_ + " SHARED src/module.cpp)\n"
               "target_include_directories(" + cls_ + " PRIVATE ${RFMOD_SDK})\n"
               "target_link_libraries(" + cls_ + " PRIVATE nlohmann_json::nlohmann_json)\n"
               "set_target_properties(" + cls_ + " PROPERTIES CXX_VISIBILITY_PRESET hidden)\n"
               "# Lets the host unload the library on upgrade (GCC only).\n"
               "if(CMAKE_CXX_COMPILER_ID STREQUAL \"GNU\")\n"
               "  target_compile_options(" + cls_ + " PRIVATE -fno-gnu-unique)\n"
               "endif()\n";
    }

    [[nodiscard]] std::string source(const json& manifest) const {
        return "#include <rfmod/module.hpp>\n\n"
               "// The manifest is compiled in so the host can check the library matches manifest.json.\n"
               "static const char* kManifest = R\"json(" + manifest.dump() + ")json\";\n\n"
               "class " + cls_ + " final : public rfmod::Module {\n"
               "public:\n"
               "    const char* manifest() const override { return kManifest; }\n\n"
               "    void init(const rfmod::HostClient&, const rfmod::json& options) override {\n"
               "        extraLossDb_ = options.value(\"extraLossDb\", 0.0);\n"
               "    }\n\n"
               "    rfmod::json invoke(const std::string& method, const rfmod::json& request) override {\n"
               "        if (method == \"loss.contribute\") {\n"
               "            (void)request;  // path context: frequency, profile summary, diffraction\n"
               "            return {{\"terms\", {{{\"id\", \"" + id_ + "/extra\"}, {\"label\", \"Extra loss\"},\n"
               "                                {\"valueDb\", extraLossDb_}, {\"source\", \"" + id_ + " options\"}}}},\n"
               "                    {\"notes\", rfmod::json::array()}};\n"
               "        }\n"
               "        throw rfmod::UnknownMethod(method);\n"
               "    }\n\n"
               "    rfmod::json selftest() override {\n"
               "        const bool ok = invoke(\"loss.contribute\", rfmod::json::object()).at(\"terms\").size() == 1;\n"
               "        return {{\"ok\", ok}, {\"checks\", {{{\"name\", \"one term\"}, {\"ok\", ok}}}}};\n"
               "    }\n\n"
               "private:\n"
               "    double extraLossDb_ = 0.0;\n"
               "};\n\n"
               "RFMOD_DEFINE_MODULE(" + cls_ + ")\n";
    }

    [[nodiscard]] std::string readme() const {
        return "# " + id_ + "\n\nBuild, stage and sign:\n\n```sh\n"
               "cmake -B build -DRFMOD_SDK=/path/to/fpv-radio-map/platform/rfmod/include && cmake --build build\n"
               "mkdir -p stage/lib && cp manifest.json stage/ && cp -r schema stage/ && cp build/lib" + cls_ +
               ".so stage/lib/\n"
               "rfmod pack stage --key me.secret\n"
               "rfmod test " + id_ + "-0.1.0.rfmod --trust me.pub\n```\n\n"
               "Remember to keep the manifest in src/module.cpp identical to manifest.json.\n"
               "See docs/modules/developer-guide.md in the rfmap repository.\n";
    }

    std::string id_;
    fs::path dir_;
    std::string cls_;
};

/// The tool: one method per command, sharing the argument and output conventions.
class Rfmod {
public:
    explicit Rfmod(Args args) : a_(std::move(args)) {}

    int run(const std::string& cmd) {
        static const std::map<std::string, int (Rfmod::*)(), std::less<>> commands = {
            {"keygen", &Rfmod::keygen},   {"new", &Rfmod::scaffold}, {"pack", &Rfmod::pack},
            {"inspect", &Rfmod::inspect}, {"check", &Rfmod::check},  {"test", &Rfmod::test},
            {"install", &Rfmod::install}, {"remove", &Rfmod::remove}, {"list", &Rfmod::list},
            {"trust", &Rfmod::trust}};
        const auto it = commands.find(cmd);
        if (it == commands.end()) {
            usage();
            return 64;
        }
        return (this->*it->second)();
    }

    static void usage() {
        std::cerr << "usage: rfmod <command> ...\n"
                     "  rfmod keygen --out NAME\n"
                     "  rfmod new ID [--dir DIR]\n"
                     "  rfmod pack DIR --key NAME.secret [-o FILE.rfmod]\n"
                     "  rfmod inspect FILE.rfmod\n"
                     "  rfmod check FILE.rfmod --trust KEY.pub [--host-version X.Y.Z]\n"
                     "  rfmod test FILE.rfmod --trust KEY.pub [--with DEP.rfmod ...] [--fixtures FILE.json]\n"
                     "  rfmod install FILE.rfmod --root DIR --trust-store KEYS.json [--fixtures FILE.json]\n"
                     "  rfmod remove ID --root DIR --trust-store KEYS.json\n"
                     "  rfmod list --root DIR --trust-store KEYS.json\n"
                     "  rfmod trust list|add KEY.pub --label TEXT|remove KEY_ID --trust-store KEYS.json\n";
    }

private:
    static int printReport(const rfmod::ValidationReport& r) {
        for (const auto& c : r.checks()) {
            std::cout << (c.ok ? "  ok    " : "  FAIL  ") << c.name << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
        }
        std::cout << (r.ok() ? "All checks passed.\n" : "Package refused.\n");
        return r.ok() ? 0 : 1;
    }

    [[nodiscard]] rfmod::Version hostVersion() const { return rfmod::Version::parse(a_.get("--host-version", kHostVersion)); }

    /// The developer's trust: --trust keys only, the first in the owner's role.
    [[nodiscard]] rfmod::TrustStore developerTrust() const {
        const auto keys = a_.all("--trust");
        if (keys.empty()) throw std::invalid_argument("give at least one --trust KEY.pub");
        rfmod::TrustStore store(Files::read(keys.front()), {});
        for (std::size_t i = 1; i < keys.size(); ++i) {
            store.add(rfmod::PublicKey::fromText(Files::read(keys[i])), fs::path(keys[i]).stem().string());
        }
        return store;
    }

    /// The operator's trust: the owner key of this build plus the server's trust store file, so
    /// third-party modules are seen as the server sees them (a missing store would hide them).
    [[nodiscard]] rfmod::TrustStore operatorTrust() const {
        if (std::string_view(kOwnerKey).empty()) throw std::invalid_argument("this rfmod was built without an owner key");
        return {kOwnerKey, a_.need("--trust-store", "give --trust-store KEYS.json: the server's modules.trustStore file")};
    }

    [[nodiscard]] FixtureHost fixtureHost() const {
        return FixtureHost(a_.get("--fixtures").empty() ? json::object() : json::parse(Files::read(a_.get("--fixtures"))));
    }

    /// rfmod-worker next to this binary (a build tree), else where `cmake --install` puts it
    /// relative to this binary (bin/rfmod and lib/rfmap/rfmod-worker).
    [[nodiscard]] static fs::path workerExecutable() {
        const fs::path self = fs::canonical("/proc/self/exe").parent_path();
        for (const fs::path& candidate : {self / "rfmod-worker", self.parent_path() / "lib" / "rfmap" / "rfmod-worker"}) {
            if (fs::is_regular_file(candidate)) return candidate;
        }
        return self / "rfmod-worker";
    }

    [[nodiscard]] rfmod::InstallerConfig installerConfig(const fs::path& root) const {
        rfmod::InstallerConfig cfg;
        cfg.root = root;
        // The worker is the one installed with this tool, never a path from the command line.
        cfg.workerExecutable = workerExecutable();
        cfg.hostVersion = hostVersion();
        return cfg;
    }

    int keygen() {
        const std::string out = a_.need("--out", "rfmod keygen --out NAME");
        if (fs::exists(out + ".secret")) throw std::invalid_argument(out + ".secret exists; refusing to overwrite a key");
        const rfmod::SecretKey sk = rfmod::SecretKey::generate();
        Files::write(out + ".secret", sk.toText() + "\n", 0600);
        Files::write(out + ".pub", sk.publicKey().toText() + "\n");
        std::cout << "Key id " << sk.publicKey().id() << "\n"
                  << "Public key:  " << out << ".pub  (give this to server admins)\n"
                  << "Secret key:  " << out << ".secret  (keep it offline; anyone with it signs as you)\n";
        return 0;
    }

    int scaffold() {
        const std::string& id = a_.only("rfmod new ID [--dir DIR]");
        const fs::path dir = a_.get("--dir", id);
        if (fs::exists(dir)) throw std::invalid_argument(dir.string() + " already exists");
        Scaffold(id, dir).write();
        std::cout << "Created " << dir.string() << "\n";
        return 0;
    }

    int pack() {
        const fs::path dir = a_.only("rfmod pack DIR --key NAME.secret");
        const json manifest = json::parse(Files::read(dir / "manifest.json"));
        const rfmod::SecretKey sk = rfmod::SecretKey::fromText(Files::read(a_.need("--key", "rfmod pack DIR --key NAME.secret")));
        const std::string out =
            a_.get("--out", manifest.value("id", "module") + "-" + manifest.value("version", "0.0.0") + ".rfmod");
        Files::write(out, rfmod::PackageWriter::pack(dir, sk));
        std::cout << "Wrote " << out << ", signed by " << sk.publicKey().id() << "\n";
        return 0;
    }

    int inspect() {
        rfmod::ValidationReport r;
        const auto files = rfmod::PackageReader::read(Files::read(a_.only("rfmod inspect FILE.rfmod")), {}, r);
        if (!files) return printReport(r);
        for (const auto& [path, content] : files->files) std::cout << "  " << content.size() << "\t" << path << "\n";
        if (files->files.contains("manifest.json")) std::cout << json::parse(files->files.at("manifest.json")).dump(2) << "\n";
        return 0;
    }

    int check() {
        const std::string& file = a_.only("rfmod check FILE.rfmod --trust KEY.pub");
        const rfmod::TrustStore trust = developerTrust();
        rfmod::ValidationReport r;
        (void)rfmod::PackageVerifier(trust, hostVersion()).verify(Files::read(file), r);
        return printReport(r);
    }

    int test() {
        const std::string& file = a_.only("rfmod test FILE.rfmod --trust KEY.pub");
        const rfmod::TrustStore trust = developerTrust();
        FixtureHost host = fixtureHost();
        const PrivateDir tmp(fs::current_path() / ".rfmod-test-");
        rfmod::ModuleInstaller installer(installerConfig(tmp.path()), trust, host);
        // Dependencies are installed first, as an operator would.
        for (const std::string& dep : a_.all("--with")) {
            if (const rfmod::ValidationReport r = installer.install(Files::read(dep)); !r.ok()) {
                std::cout << "Dependency " << dep << " was refused:\n";
                return printReport(r);
            }
        }
        return printReport(installer.check(Files::read(file)));
    }

    int install() {
        const std::string& file = a_.only("rfmod install FILE.rfmod --root DIR --trust-store KEYS.json");
        const rfmod::TrustStore trust = operatorTrust();
        FixtureHost host = fixtureHost();
        rfmod::ModuleInstaller installer(installerConfig(a_.need("--root", "rfmod install FILE.rfmod --root DIR --trust-store KEYS.json")), trust, host);
        const int rc = printReport(installer.install(Files::read(file)));
        if (rc == 0) std::cout << "Ask the server to pick it up: POST /api/v1/admin/modules/reload\n";
        return rc;
    }

    int remove() {
        const std::string& id = a_.only("rfmod remove ID --root DIR --trust-store KEYS.json");
        const rfmod::TrustStore trust = operatorTrust();
        FixtureHost host = fixtureHost();
        rfmod::ModuleInstaller(installerConfig(a_.need("--root", "rfmod remove ID --root DIR --trust-store KEYS.json")), trust, host).remove(id);
        std::cout << "Removed " << id << ". Ask the server to pick it up: POST /api/v1/admin/modules/reload\n";
        return 0;
    }

    int list() {
        const rfmod::TrustStore trust = operatorTrust();
        FixtureHost host = fixtureHost();
        const rfmod::ModuleInstaller installer(installerConfig(a_.need("--root", "rfmod list --root DIR --trust-store KEYS.json")), trust, host);
        for (const auto& [id, m] : installer.installed()) {
            std::cout << id << " " << m.manifest.version.str() << (m.settings.enabled ? "" : " (disabled)")
                      << "  signed by " << m.signer.label << " (" << m.signer.key.id() << ")\n";
        }
        return 0;
    }

    int trust() {
        const auto& words = a_.positional();
        const char* usage = "rfmod trust list|add KEY.pub --label TEXT|remove KEY_ID --trust-store KEYS.json";
        if (words.empty()) throw std::invalid_argument(usage);
        rfmod::TrustStore store(kOwnerKey, a_.need("--trust-store", usage));
        if (words[0] == "add" && words.size() == 2) {
            const rfmod::TrustedKey& k = store.add(rfmod::PublicKey::fromText(Files::read(words[1])), a_.need("--label", usage));
            std::cout << "Trusted " << k.key.id() << " (" << k.label << ")\n";
        } else if (words[0] == "remove" && words.size() == 2) {
            store.remove(words[1]);
            std::cout << "Removed " << words[1] << "; its modules stop loading at the next reload\n";
        } else if (words[0] == "list" && words.size() == 1) {
            for (const rfmod::TrustedKey& k : store.keys()) {
                std::cout << k.key.id() << "  " << k.label << (k.builtIn ? "  (built in)" : "") << "\n";
            }
        } else {
            throw std::invalid_argument(usage);
        }
        return 0;
    }

    Args a_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Rfmod::usage();
        return 64;
    }
    const std::string cmd = argv[1];
    try {
        return Rfmod(Args(std::span<char* const>(argv, static_cast<std::size_t>(argc)).subspan(2))).run(cmd);
    } catch (const std::exception& e) {
        std::cerr << "rfmod " << cmd << ": " << e.what() << "\n";
        return 1;
    }
}
