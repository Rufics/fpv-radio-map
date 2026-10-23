#pragma once

#include "rfmod/confined_root.hpp"
#include "rfmod/manifest.hpp"
#include "rfmod/package.hpp"
#include "rfmod/trust.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

/// A file in the module root could not be written.
class RegistryWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The admin's settings for an installed module, kept in state.json next to it.
struct ModuleSettings {
    bool enabled = true;
    nlohmann::json options = nlohmann::json::object();
};

/// The directory layout of a module root and every file operation on it. Nothing here runs
/// module code or decides policy; it only keeps the layout and the confinement in one place:
///
///   <root>/installed/<id>/package.rfmod   the signed package as installed
///   <root>/installed/<id>/state.json      ModuleSettings
///   <root>/installed/<id>/<files>         the unpacked package
///   <root>/staging/<random>/              a package being self-tested
class ModuleStore {
public:
    explicit ModuleStore(const std::filesystem::path& root);

    [[nodiscard]] const ConfinedRoot& root() const { return root_; }
    /// The install directories present, in name order.
    [[nodiscard]] std::vector<std::filesystem::path> installedDirs() const;
    [[nodiscard]] std::filesystem::path installedDir(const std::string& id) const;
    [[nodiscard]] bool isInstalled(const std::string& id) const;

    /// A new, empty staging directory.
    [[nodiscard]] std::filesystem::path newStagingDir() const;
    /// Removes every staging directory: only the installer, which owns them, calls this.
    void clearStaging() const;

    [[nodiscard]] std::string readFile(const std::filesystem::path& p) const;
    /// Writes `p` (which must lie inside the root) through a temporary file renamed into place.
    void writeFileAtomic(const std::filesystem::path& p, std::string_view content) const;
    /// Writes the package files into `dir`, each renamed into place (a library that is still
    /// mapped by a running module keeps its old pages).
    void unpack(const PackageFiles& files, const std::filesystem::path& dir) const;
    /// Whether every signed file is in `dir` with the signed content.
    [[nodiscard]] bool intact(const PackageFiles& files, const std::filesystem::path& dir) const;

    [[nodiscard]] std::string readPackage(const std::filesystem::path& dir) const;
    void writePackage(const std::filesystem::path& dir, std::string_view archive) const;
    /// The settings in `dir`, or the defaults with `problem` set when state.json is unreadable.
    [[nodiscard]] ModuleSettings readSettings(const std::filesystem::path& dir, std::string& problem) const;
    void writeSettings(const std::filesystem::path& dir, const ModuleSettings& settings, const Manifest& manifest,
                       const TrustedKey& signer) const;

    /// Moves the staged directory into installed/<id>, replacing what was there.
    void replace(const std::filesystem::path& staged, const std::string& id) const;
    void remove(const std::filesystem::path& dir) const;

private:
    ConfinedRoot root_;
};

}  // namespace rfmod
