#include "rfmod/installed.hpp"

namespace rfmod {
namespace fs = std::filesystem;

std::optional<InstalledModule> InstalledReader::read(const fs::path& dir, std::vector<std::string>& notices) const {
    ValidationReport report;
    auto pkg = verifier_.verify(store_.readPackage(dir), report);
    if (!pkg) {
        std::string why;
        for (const auto& c : report.checks()) {
            if (!c.ok) why = c.name + ": " + c.detail;
        }
        notices.push_back("Module in " + dir.filename().string() + " was not loaded. " + why);
        return std::nullopt;
    }
    std::string problem;
    InstalledModule m{pkg->manifest, pkg->signer, dir, store_.readSettings(dir, problem)};
    if (!problem.empty()) notices.push_back(m.manifest.id + ": " + problem);
    if (!store_.intact(pkg->files, dir)) {
        store_.unpack(pkg->files, dir);
        notices.push_back(m.manifest.id + ": unpacked files differed from the signed package and were restored");
    }
    return m;
}

std::map<std::string, InstalledModule, std::less<>> InstalledReader::all(std::vector<std::string>& notices) const {
    std::map<std::string, InstalledModule, std::less<>> out;
    for (const fs::path& dir : store_.installedDirs()) {
        if (auto m = read(dir, notices)) {
            const std::string id = m->manifest.id;
            if (id != dir.filename().string()) {
                notices.push_back("Module in " + dir.filename().string() + " was not loaded. Its manifest id is " + id);
                continue;
            }
            out.try_emplace(id, std::move(*m));
        }
    }
    return out;
}

}  // namespace rfmod
