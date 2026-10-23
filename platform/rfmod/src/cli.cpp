#include "rfmod/cli.hpp"

#include "rfmod/installer.hpp"
#include "rfmod/package.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace rfmod {

/// The command line: positional words and --flag VALUE pairs.
class CliArgs {
public:
    explicit CliArgs(std::span<const std::string> words) {
        auto next = words.begin();
        while (next != words.end()) {
            const std::string& s = *next++;
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

namespace {

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

    /// Creates or replaces `p` with `mode` applied before any content is written, so a secret
    /// is never readable by others for an instant. `exclusive` refuses an existing file: the
    /// creation itself is the check, and no other process can slip a file in between.
    static void write(const fs::path& p, const std::string& content, mode_t mode = 0644, bool exclusive = false) {
        if (p.has_parent_path()) fs::create_directories(p.parent_path());
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
        const int fd = ::open(p.c_str(), flags, mode);
        if (fd < 0 && exclusive && errno == EEXIST) throw FileError(p.string() + " exists; refusing to overwrite it");
        if (fd < 0) throw FileError("cannot write " + p.string() + ": " + std::strerror(errno));
        const bool written = ::fchmod(fd, mode) == 0 && writeAll(fd, content);
        const bool closed = ::close(fd) == 0;
        if (!written || !closed) throw FileError("cannot write " + p.string());
    }

    static bool writeAll(int fd, std::string_view content) {
        while (!content.empty()) {
            const ssize_t n = ::write(fd, content.data(), content.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            content.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }
};

/// The developer's trust: the --trust keys only, the first in the owner's role.
class DeveloperTrust {
public:
    explicit DeveloperTrust(const std::vector<std::string>& keyFiles) : store_(Files::read(ownerKeyFile(keyFiles)), {}) {
        for (const std::string& file : keyFiles | std::views::drop(1)) {
            store_.add(PublicKey::fromText(Files::read(file)), fs::path(file).stem().string());
        }
    }

    [[nodiscard]] const TrustStore& store() const { return store_; }

private:
    static const std::string& ownerKeyFile(const std::vector<std::string>& keyFiles) {
        if (keyFiles.empty()) throw std::invalid_argument("give at least one --trust KEY.pub");
        return keyFiles.front();
    }

    TrustStore store_;
};

/// Answers a self-test's host calls from a fixtures file and prints its log lines.
class FixtureHost final : public IHostServices {
public:
    FixtureHost(json fixtures, std::ostream& log) : fixtures_(std::move(fixtures)), log_(log) {}
    json call(const std::string&, const std::string& method, const json&) override {
        if (fixtures_.contains(method)) return fixtures_.at(method);
        throw MissingFixture(method);
    }
    void log(const std::string& id, int level, const std::string& message) override {
        static constexpr std::array<const char*, 4> names = {"debug", "info", "warn", "error"};
        log_ << "[" << id << " " << names.at(static_cast<std::size_t>(std::clamp(level, 0, 3))) << "] " << message
             << "\n";
    }

    /// The host for the --fixtures file of `args`, or one that answers nothing.
    [[nodiscard]] static FixtureHost from(const CliArgs& args, std::ostream& log) {
        const std::string file = args.get("--fixtures");
        return {file.empty() ? json::object() : json::parse(Files::read(file)), log};
    }

private:
    json fixtures_;
    std::ostream& log_;
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

constexpr const char* kReloadHint = "Ask the server to pick it up: POST /api/v1/admin/modules/reload\n";

}  // namespace

Cli::Cli(CliBuild build, std::ostream& out, std::ostream& err) : build_(std::move(build)), out_(out), err_(err) {}

void Cli::usage(std::ostream& err) {
    err << "usage: rfmod <command> ...\n"
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

int Cli::run(std::span<const std::string> words) const {
    if (words.empty()) {
        usage(err_);
        return 64;
    }
    const std::string& command = words.front();
    try {
        return dispatch(command, CliArgs(words.subspan(1)));
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): the tool's boundary, every failure is printed
        err_ << "rfmod " << command << ": " << e.what() << "\n";
        return 1;
    }
}

int Cli::dispatch(const std::string& command, const CliArgs& args) const {
    static const std::map<std::string, int (Cli::*)(const CliArgs&) const, std::less<>> commands = {
        {"keygen", &Cli::keygen},   {"new", &Cli::scaffold}, {"pack", &Cli::pack},     {"inspect", &Cli::inspect},
        {"check", &Cli::check},     {"test", &Cli::test},    {"install", &Cli::install}, {"remove", &Cli::remove},
        {"list", &Cli::list},       {"trust", &Cli::trust}};
    const auto it = commands.find(command);
    if (it == commands.end()) {
        usage(err_);
        return 64;
    }
    return (this->*it->second)(args);
}

int Cli::printReport(const ValidationReport& r) const {
    for (const auto& c : r.checks()) {
        out_ << (c.ok ? "  ok    " : "  FAIL  ") << c.name << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
    }
    out_ << (r.ok() ? "All checks passed.\n" : "Package refused.\n");
    return r.ok() ? 0 : 1;
}

Version Cli::hostVersion(const CliArgs& args) const { return Version::parse(args.get("--host-version", build_.hostVersion)); }

/// The operator's trust: the owner key of this build plus the server's trust store file, so
/// third-party modules are seen as the server sees them (a missing store would hide them).
TrustStore Cli::operatorTrust(const CliArgs& args) const {
    if (build_.ownerKey.empty()) throw std::invalid_argument("this rfmod was built without an owner key");
    return {build_.ownerKey, args.need("--trust-store", "give --trust-store KEYS.json: the server's modules.trustStore file")};
}

InstallerConfig Cli::installerConfig(const CliArgs& args, const fs::path& root) const {
    InstallerConfig cfg;
    cfg.root = root;
    // The worker is the one installed with this tool, never a path from the command line.
    cfg.workerExecutable = build_.workerExecutable;
    cfg.hostVersion = hostVersion(args);
    return cfg;
}

int Cli::keygen(const CliArgs& args) const {
    const std::string out = args.need("--out", "rfmod keygen --out NAME");
    // No separate existence check: the exclusive create below is the check, with no window.
    const SecretKey sk = SecretKey::generate();
    Files::write(out + ".secret", sk.toText() + "\n", 0600, true);
    Files::write(out + ".pub", sk.publicKey().toText() + "\n");
    out_ << "Key id " << sk.publicKey().id() << "\n"
         << "Public key:  " << out << ".pub  (give this to server admins)\n"
         << "Secret key:  " << out << ".secret  (keep it offline; anyone with it signs as you)\n";
    return 0;
}

int Cli::scaffold(const CliArgs& args) const {
    const std::string& id = args.only("rfmod new ID [--dir DIR]");
    // The id names the directory, the class and the manifest: only a real module id will do.
    if (!ModuleInstaller::looksLikeModuleId(id)) {
        throw std::invalid_argument(id + " is not a module id (lower-case words joined by dots, e.g. acme.rain)");
    }
    const fs::path dir = args.get("--dir", id);
    if (fs::exists(dir)) throw std::invalid_argument(dir.string() + " already exists");
    Scaffold(id, dir).write();
    out_ << "Created " << dir.string() << "\n";
    return 0;
}

int Cli::pack(const CliArgs& args) const {
    const fs::path dir = args.only("rfmod pack DIR --key NAME.secret");
    const json manifest = json::parse(Files::read(dir / "manifest.json"));
    const SecretKey sk = SecretKey::fromText(Files::read(args.need("--key", "rfmod pack DIR --key NAME.secret")));
    const std::string out =
        args.get("--out", manifest.value("id", "module") + "-" + manifest.value("version", "0.0.0") + ".rfmod");
    Files::write(out, PackageWriter::pack(dir, sk));
    out_ << "Wrote " << out << ", signed by " << sk.publicKey().id() << "\n";
    return 0;
}

int Cli::inspect(const CliArgs& args) const {
    ValidationReport r;
    const auto files = PackageReader::read(Files::read(args.only("rfmod inspect FILE.rfmod")), {}, r);
    if (!files) return printReport(r);
    for (const auto& [path, content] : files->files) out_ << "  " << content.size() << "\t" << path << "\n";
    if (files->files.contains("manifest.json")) out_ << json::parse(files->files.at("manifest.json")).dump(2) << "\n";
    return 0;
}

int Cli::check(const CliArgs& args) const {
    const std::string& file = args.only("rfmod check FILE.rfmod --trust KEY.pub");
    const DeveloperTrust developer(args.all("--trust"));
    const TrustStore& trust = developer.store();
    ValidationReport r;
    (void)PackageVerifier(trust, hostVersion(args)).verify(Files::read(file), r);
    return printReport(r);
}

int Cli::test(const CliArgs& args) const {
    const std::string& file = args.only("rfmod test FILE.rfmod --trust KEY.pub");
    const DeveloperTrust developer(args.all("--trust"));
    const TrustStore& trust = developer.store();
    FixtureHost host = FixtureHost::from(args, err_);
    const PrivateDir tmp(fs::current_path() / ".rfmod-test-");
    const ModuleInstaller installer(installerConfig(args, tmp.path()), trust, host);
    // Dependencies are installed first, as an operator would.
    for (const std::string& dep : args.all("--with")) {
        if (const ValidationReport r = installer.install(Files::read(dep)); !r.ok()) {
            out_ << "Dependency " << dep << " was refused:\n";
            return printReport(r);
        }
    }
    return printReport(installer.check(Files::read(file)));
}

int Cli::install(const CliArgs& args) const {
    const char* usage = "rfmod install FILE.rfmod --root DIR --trust-store KEYS.json";
    const std::string& file = args.only(usage);
    const TrustStore trust = operatorTrust(args);
    FixtureHost host = FixtureHost::from(args, err_);
    const ModuleInstaller installer(installerConfig(args, args.need("--root", usage)), trust, host);
    const int rc = printReport(installer.install(Files::read(file)));
    if (rc == 0) out_ << kReloadHint;
    return rc;
}

int Cli::remove(const CliArgs& args) const {
    const char* usage = "rfmod remove ID --root DIR --trust-store KEYS.json";
    const std::string& id = args.only(usage);
    const TrustStore trust = operatorTrust(args);
    FixtureHost host = FixtureHost::from(args, err_);
    ModuleInstaller(installerConfig(args, args.need("--root", usage)), trust, host).remove(id);
    out_ << "Removed " << id << ". " << kReloadHint;
    return 0;
}

int Cli::list(const CliArgs& args) const {
    const TrustStore trust = operatorTrust(args);
    FixtureHost host = FixtureHost::from(args, err_);
    const ModuleInstaller installer(installerConfig(args, args.need("--root", "rfmod list --root DIR --trust-store KEYS.json")),
                                    trust, host);
    for (const auto& [id, m] : installer.installed()) {
        out_ << id << " " << m.manifest.version.str() << (m.settings.enabled ? "" : " (disabled)") << "  signed by "
             << m.signer.label << " (" << m.signer.key.id() << ")\n";
    }
    return 0;
}

int Cli::trust(const CliArgs& args) const {
    const auto& words = args.positional();
    const char* usage = "rfmod trust list|add KEY.pub --label TEXT|remove KEY_ID --trust-store KEYS.json";
    if (words.empty()) throw std::invalid_argument(usage);
    TrustStore store(build_.ownerKey, args.need("--trust-store", usage));
    if (words[0] == "add" && words.size() == 2) {
        const TrustedKey& k = store.add(PublicKey::fromText(Files::read(words[1])), args.need("--label", usage));
        out_ << "Trusted " << k.key.id() << " (" << k.label << ")\n";
    } else if (words[0] == "remove" && words.size() == 2) {
        store.remove(words[1]);
        out_ << "Removed " << words[1] << "; its modules stop loading at the next reload\n";
    } else if (words[0] == "list" && words.size() == 1) {
        for (const TrustedKey& k : store.keys()) {
            out_ << k.key.id() << "  " << k.label << (k.builtIn ? "  (built in)" : "") << "\n";
        }
    } else {
        throw std::invalid_argument(usage);
    }
    return 0;
}

}  // namespace rfmod
