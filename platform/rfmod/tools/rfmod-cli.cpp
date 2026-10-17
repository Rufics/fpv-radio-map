// rfmod: the module developer tool.
//
//   rfmod keygen --out NAME              write NAME.pub and NAME.secret (keep the secret offline)
//   rfmod new ID [--dir DIR]             scaffold a module project
//   rfmod pack DIR --key NAME.secret [-o FILE.rfmod]
//   rfmod inspect FILE.rfmod
//   rfmod check FILE.rfmod --trust KEY.pub [--trust ...] [--host-version X.Y.Z]
//   rfmod test FILE.rfmod --trust KEY.pub [--worker PATH] [--fixtures FILE.json]
//
// "check" runs every upload check except the self-test; "test" also runs the self-test in a
// worker exactly as the server does. Host calls are answered from --fixtures
// ({"method": response, ...}); anything else fails, so self-tests should not depend on them.

#include "rfmod/package.hpp"
#include "rfmod/registry.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#ifndef RFMAP_HOST_VERSION
#define RFMAP_HOST_VERSION "0.1.0"
#endif

namespace fs = std::filesystem;
using rfmod::json;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::multimap<std::string, std::string> flags;

    [[nodiscard]] std::string get(const std::string& k, const std::string& fallback = {}) const {
        const auto it = flags.find(k);
        return it == flags.end() ? fallback : it->second;
    }
    [[nodiscard]] std::vector<std::string> all(const std::string& k) const {
        std::vector<std::string> out;
        for (auto [a, b] = flags.equal_range(k); a != b; ++a) out.push_back(a->second);
        return out;
    }
};

Args parseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0 || s == "-o") {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + s);
            a.flags.emplace(s == "-o" ? "--out" : s, argv[++i]);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const fs::path& p, const std::string& content, mode_t mode = 0644) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out.close();
    chmod(p.c_str(), mode);
}

int printReport(const rfmod::ValidationReport& r) {
    for (const auto& c : r.checks()) {
        std::cout << (c.ok ? "  ok    " : "  FAIL  ") << c.name << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
    }
    std::cout << (r.ok() ? "All checks passed.\n" : "Package refused.\n");
    return r.ok() ? 0 : 1;
}

rfmod::TrustStore trustFrom(const Args& a) {
    const auto keys = a.all("--trust");
    if (keys.empty()) throw std::invalid_argument("give at least one --trust KEY.pub");
    // The first key plays the owner's part, the way RFMAP_OWNER_PUBKEY does on a server.
    rfmod::TrustStore store(readFile(keys.front()), {});
    for (std::size_t i = 1; i < keys.size(); ++i) {
        store.add(rfmod::PublicKey::fromText(readFile(keys[i])), fs::path(keys[i]).stem().string());
    }
    return store;
}

class FixtureHost final : public rfmod::IHostServices {
public:
    explicit FixtureHost(json fixtures) : fixtures_(std::move(fixtures)) {}
    json call(const std::string&, const std::string& method, const json&) override {
        if (fixtures_.contains(method)) return fixtures_.at(method);
        throw std::runtime_error(method + " is not available in rfmod test (add it to --fixtures)");
    }
    void log(const std::string& id, int level, const std::string& message) override {
        static const char* names[] = {"debug", "info", "warn", "error"};
        std::cerr << "[" << id << " " << names[std::clamp(level, 0, 3)] << "] " << message << "\n";
    }

private:
    json fixtures_;
};

int cmdKeygen(const Args& a) {
    const std::string out = a.get("--out");
    if (out.empty()) throw std::invalid_argument("rfmod keygen --out NAME");
    if (fs::exists(out + ".secret")) throw std::invalid_argument(out + ".secret exists; refusing to overwrite a key");
    const rfmod::SecretKey sk = rfmod::SecretKey::generate();
    writeFile(out + ".secret", sk.toText() + "\n", 0600);
    writeFile(out + ".pub", sk.publicKey().toText() + "\n");
    std::cout << "Key id " << sk.publicKey().id() << "\n"
              << "Public key:  " << out << ".pub  (give this to server admins)\n"
              << "Secret key:  " << out << ".secret  (keep it offline; anyone with it signs as you)\n";
    return 0;
}

int cmdPack(const Args& a) {
    if (a.positional.size() != 1 || a.get("--key").empty()) throw std::invalid_argument("rfmod pack DIR --key NAME.secret");
    const fs::path dir = a.positional[0];
    const json manifest = json::parse(readFile(dir / "manifest.json"));
    const rfmod::SecretKey sk = rfmod::SecretKey::fromText(readFile(a.get("--key")));
    const std::string out =
        a.get("--out", manifest.value("id", "module") + "-" + manifest.value("version", "0.0.0") + ".rfmod");
    writeFile(out, rfmod::PackageWriter::pack(dir, sk));
    std::cout << "Wrote " << out << ", signed by " << sk.publicKey().id() << "\n";
    return 0;
}

int cmdInspect(const Args& a) {
    if (a.positional.size() != 1) throw std::invalid_argument("rfmod inspect FILE.rfmod");
    rfmod::ValidationReport r;
    const auto files = rfmod::PackageReader::read(readFile(a.positional[0]), {}, r);
    if (!files) return printReport(r);
    for (const auto& [path, content] : files->files) std::cout << "  " << content.size() << "\t" << path << "\n";
    if (files->files.count("manifest.json")) std::cout << json::parse(files->files.at("manifest.json")).dump(2) << "\n";
    return 0;
}

int cmdCheck(const Args& a) {
    if (a.positional.size() != 1) throw std::invalid_argument("rfmod check FILE.rfmod --trust KEY.pub");
    const rfmod::TrustStore trust = trustFrom(a);
    rfmod::ValidationReport r;
    (void)rfmod::PackageVerifier::verify(readFile(a.positional[0]), trust,
                                         rfmod::Version::parse(a.get("--host-version", RFMAP_HOST_VERSION)), r);
    return printReport(r);
}

int cmdTest(const Args& a, const char* argv0) {
    if (a.positional.size() != 1) throw std::invalid_argument("rfmod test FILE.rfmod --trust KEY.pub");
    const rfmod::TrustStore trust = trustFrom(a);
    FixtureHost host(a.get("--fixtures").empty() ? json::object() : json::parse(readFile(a.get("--fixtures"))));
    const fs::path tmp = fs::temp_directory_path() / ("rfmod-test-" + std::to_string(getpid()));
    rfmod::RegistryConfig cfg;
    cfg.root = tmp;
    cfg.workerExecutable =
        a.get("--worker", (fs::canonical("/proc/self/exe").parent_path() / "rfmod-worker").string());
    (void)argv0;
    cfg.hostVersion = rfmod::Version::parse(a.get("--host-version", RFMAP_HOST_VERSION));
    int rc = 1;
    {
        rfmod::ModuleRegistry registry(cfg, trust, host);
        rc = printReport(registry.upload(readFile(a.positional[0]), false));
    }
    fs::remove_all(tmp);
    return rc;
}

int cmdNew(const Args& a) {
    if (a.positional.size() != 1) throw std::invalid_argument("rfmod new ID [--dir DIR]");
    const std::string id = a.positional[0];
    const fs::path dir = a.get("--dir", id);
    if (fs::exists(dir)) throw std::invalid_argument(dir.string() + " already exists");
    std::string cls;
    bool up = true;
    for (char c : id) {
        if (c == '.' || c == '-') {
            up = true;
            continue;
        }
        cls += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    cls += "Module";
    const json manifest = {{"schema", "rfmod/v1"},
                           {"id", id},
                           {"version", "0.1.0"},
                           {"title", "My module"},
                           {"description", "What this module adds to the link budget."},
                           {"author", ""},
                           {"license", "MIT"},
                           {"lifecycle", "alpha"},
                           {"abi", 1},
                           {"host", ">=0.1.0 <1.0.0"},
                           {"requires", json::array()},
                           {"provides", {"loss.contributor"}},
                           {"methods", {"loss.contribute"}},
                           {"entry", {{"linux-x86_64", "lib/lib" + cls + ".so"}}},
                           {"optionsSchema", "schema/options.json"},
                           {"limits", {{"memoryMb", 256}, {"timeoutMs", 5000}}}};
    writeFile(dir / "manifest.json", manifest.dump(2) + "\n");
    writeFile(dir / "schema/options.json",
              json{{"$schema", "http://json-schema.org/draft-07/schema#"},
                   {"type", "object"},
                   {"additionalProperties", false},
                   {"properties", {{"extraLossDb", {{"type", "number"}, {"minimum", 0}, {"default", 0}}}}}}
                      .dump(2) + "\n");
    writeFile(dir / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + cls + " LANGUAGES CXX)\n"
              "set(CMAKE_CXX_STANDARD 20)\n"
              "find_package(nlohmann_json REQUIRED)\n"
              "# Point RFMOD_SDK at platform/rfmod/include of the rfmap source tree.\n"
              "set(RFMOD_SDK \"\" CACHE PATH \"rfmod SDK include directory\")\n"
              "add_library(" + cls + " SHARED src/module.cpp)\n"
              "target_include_directories(" + cls + " PRIVATE ${RFMOD_SDK})\n"
              "target_link_libraries(" + cls + " PRIVATE nlohmann_json::nlohmann_json)\n"
              "set_target_properties(" + cls + " PROPERTIES CXX_VISIBILITY_PRESET hidden)\n"
              "# Lets the host unload the library on upgrade (GCC only).\n"
              "if(CMAKE_CXX_COMPILER_ID STREQUAL \"GNU\")\n"
              "  target_compile_options(" + cls + " PRIVATE -fno-gnu-unique)\n"
              "endif()\n");
    writeFile(dir / "src/module.cpp",
              "#include <rfmod/module.hpp>\n\n"
              "// The manifest is compiled in so the host can check the library matches manifest.json.\n"
              "static const char* kManifest = R\"json(" + manifest.dump() + ")json\";\n\n"
              "class " + cls + " final : public rfmod::Module {\n"
              "public:\n"
              "    const char* manifest() const override { return kManifest; }\n\n"
              "    void init(const rfmod::HostClient&, const rfmod::json& options) override {\n"
              "        extraLossDb_ = options.value(\"extraLossDb\", 0.0);\n"
              "    }\n\n"
              "    rfmod::json invoke(const std::string& method, const rfmod::json& request) override {\n"
              "        if (method == \"loss.contribute\") {\n"
              "            (void)request;  // path context: frequency, profile summary, diffraction\n"
              "            return {{\"terms\", {{{\"id\", \"" + id + "/extra\"}, {\"label\", \"Extra loss\"},\n"
              "                                {\"valueDb\", extraLossDb_}, {\"source\", \"" + id + " options\"}}}},\n"
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
              "RFMOD_DEFINE_MODULE(" + cls + ")\n");
    writeFile(dir / "README.md",
              "# " + id + "\n\nBuild, stage and sign:\n\n```sh\n"
              "cmake -B build -DRFMOD_SDK=/path/to/fpv-radio-map/platform/rfmod/include && cmake --build build\n"
              "mkdir -p stage/lib && cp manifest.json stage/ && cp -r schema stage/ && cp build/lib" + cls + ".so stage/lib/\n"
              "rfmod pack stage --key me.secret\n"
              "rfmod test " + id + "-0.1.0.rfmod --trust me.pub\n```\n\n"
              "Remember to keep the manifest in src/module.cpp identical to manifest.json.\n"
              "See docs/modules/developer-guide.md in the rfmap repository.\n");
    std::cout << "Created " << dir.string() << "\n";
    return 0;
}

void usage() {
    std::cerr << "usage: rfmod <keygen|new|pack|inspect|check|test> ...\n"
                 "  rfmod keygen --out NAME\n"
                 "  rfmod new ID [--dir DIR]\n"
                 "  rfmod pack DIR --key NAME.secret [-o FILE.rfmod]\n"
                 "  rfmod inspect FILE.rfmod\n"
                 "  rfmod check FILE.rfmod --trust KEY.pub [--host-version X.Y.Z]\n"
                 "  rfmod test FILE.rfmod --trust KEY.pub [--worker PATH] [--fixtures FILE.json]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 64;
    }
    const std::string cmd = argv[1];
    try {
        const Args a = parseArgs(argc, argv, 2);
        if (cmd == "keygen") return cmdKeygen(a);
        if (cmd == "new") return cmdNew(a);
        if (cmd == "pack") return cmdPack(a);
        if (cmd == "inspect") return cmdInspect(a);
        if (cmd == "check") return cmdCheck(a);
        if (cmd == "test") return cmdTest(a, argv[0]);
        usage();
        return 64;
    } catch (const std::exception& e) {
        std::cerr << "rfmod " << cmd << ": " << e.what() << "\n";
        return 1;
    }
}
