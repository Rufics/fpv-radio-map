#include "rfmod/module_store.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <random>
#include <sstream>

namespace rfmod {
namespace fs = std::filesystem;

namespace {

constexpr const char* kPackageFile = "package.rfmod";
constexpr const char* kStateFile = "state.json";

std::string randomName() {
    std::random_device rd;
    std::ostringstream s;
    s << std::hex << rd() << rd();
    return s.str();
}

std::string isoNow() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

}  // namespace

ModuleStore::ModuleStore(const fs::path& root) : root_(root) {
    fs::create_directories(root_.resolve("installed"));
    fs::create_directories(root_.resolve("staging"));
}

std::vector<fs::path> ModuleStore::installedDirs() const {
    std::vector<fs::path> dirs;
    for (const auto& item : fs::directory_iterator(root_.resolve("installed"))) {
        if (item.is_directory()) dirs.push_back(item.path());
    }
    std::ranges::sort(dirs);
    return dirs;
}

fs::path ModuleStore::installedDir(const std::string& id) const { return root_.resolve(fs::path("installed") / id); }

bool ModuleStore::isInstalled(const std::string& id) const { return fs::is_directory(installedDir(id)); }

fs::path ModuleStore::newStagingDir() const {
    const fs::path dir = root_.resolve(fs::path("staging") / randomName());
    fs::create_directories(dir);
    return dir;
}

void ModuleStore::clearStaging() const {
    const fs::path staging = root_.resolve("staging");
    fs::remove_all(staging);
    fs::create_directories(staging);
}

std::string ModuleStore::readFile(const fs::path& p) const {
    std::ifstream in(root_.confine(p), std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void ModuleStore::writeFileAtomic(const fs::path& p, std::string_view content) const {
    const fs::path target = root_.confine(p);
    const fs::path tmp = root_.confine(target.string() + ".tmp");
    fs::create_directories(target.parent_path());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw RegistryWriteError("cannot write " + tmp.string());
    }
    fs::rename(tmp, target);
}

void ModuleStore::unpack(const PackageFiles& files, const fs::path& dir) const {
    for (const auto& [path, content] : files.files) writeFileAtomic(dir / path, content);
}

bool ModuleStore::intact(const PackageFiles& files, const fs::path& dir) const {
    return std::ranges::all_of(files.files, [this, &dir](const auto& file) {
        const fs::path p = dir / file.first;
        return fs::is_regular_file(p) && readFile(p) == file.second;
    });
}

std::string ModuleStore::readPackage(const fs::path& dir) const { return readFile(dir / kPackageFile); }

void ModuleStore::writePackage(const fs::path& dir, std::string_view archive) const {
    writeFileAtomic(dir / kPackageFile, archive);
}

ModuleSettings ModuleStore::readSettings(const fs::path& dir, std::string& problem) const {
    ModuleSettings s;
    const fs::path file = dir / kStateFile;
    if (!fs::exists(file)) return s;
    try {
        const nlohmann::json state = nlohmann::json::parse(readFile(file));
        s.enabled = state.value("enabled", true);
        s.options = state.value("options", nlohmann::json::object());
        if (!s.options.is_object()) {
            s.options = nlohmann::json::object();
            problem = "state.json options were not an object and were reset";
        }
    } catch (const nlohmann::json::exception& e) {
        problem = std::string("state.json is unreadable and the defaults apply: ") + e.what();
    }
    return s;
}

void ModuleStore::writeSettings(const fs::path& dir, const ModuleSettings& settings, const Manifest& manifest,
                                const TrustedKey& signer) const {
    const nlohmann::json state = {{"enabled", settings.enabled},
                                  {"options", settings.options},
                                  {"signer", signer.key.id()},
                                  {"version", manifest.version.str()},
                                  {"updatedAt", isoNow()}};
    writeFileAtomic(dir / kStateFile, state.dump(2) + "\n");
}

void ModuleStore::replace(const fs::path& staged, const std::string& id) const {
    const fs::path target = installedDir(id);
    const fs::path source = root_.confine(staged);
    const fs::path old = root_.resolve(fs::path("staging") / (id + ".old." + randomName()));
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(source, target);
    fs::remove_all(old);
}

void ModuleStore::remove(const fs::path& dir) const { fs::remove_all(root_.confine(dir)); }

}  // namespace rfmod
