#pragma once

#include "rfmod/confined_root.hpp"
#include "rfmod/host.hpp"
#include "rfmod/package.hpp"
#include "rfmod/trust.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

/// A file in the registry directory could not be written.
class RegistryWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ModuleState { Active, Disabled, MissingDependency, Failed };
std::string toString(ModuleState s);

struct RegistryConfig {
    std::filesystem::path root;               ///< <root>/installed/<id>/..., <root>/staging/
    std::filesystem::path workerExecutable;   ///< rfmod-worker
    Version hostVersion;
    bool isolateAll = false;                  ///< Run first-party modules in workers too.
    PackageLimits limits;
};

/// Installed modules: upload validation, dependency resolution, deprecation notices, loading.
/// Only the admin API reaches the mutating calls; the server enforces that.
class ModuleRegistry {
public:
    struct Entry {
        Manifest manifest;
        TrustedKey signer;
        std::filesystem::path dir;
        bool enabled = true;
        json options = json::object();
        ModuleState state = ModuleState::Disabled;
        std::string stateDetail;
        std::unique_ptr<IModuleInstance> instance;
    };

    ModuleRegistry(RegistryConfig config, const TrustStore& trust, IHostServices& services);
    ~ModuleRegistry();

    /// Validates an uploaded package and runs its self-test in a worker. Installs it only when
    /// every check passed and `install` is true. Replaces an older version of the same module.
    ValidationReport upload(const std::string& archive, bool install);

    /// Throws std::invalid_argument if another enabled module requires it.
    void remove(const std::string& id);
    void setEnabled(const std::string& id, bool enabled);
    /// Validates against the module's optionsSchema; throws std::invalid_argument with the reason.
    void setOptions(const std::string& id, const json& options);

    /// Re-reads <root>/installed, re-verifies every package against the current trust store,
    /// resolves dependencies and (re)loads modules in dependency order.
    void reload();

    [[nodiscard]] json list() const;  ///< OpenAPI ModuleInfo[]
    [[nodiscard]] std::vector<std::string> notices() const;
    /// Active modules providing a capability, in dependency order. Pointers stay valid until reload().
    [[nodiscard]] std::vector<IModuleInstance*> withCapability(const std::string& capability) const;
    [[nodiscard]] IModuleInstance* instance(const std::string& id) const;

    /// Opens a module's UI bundle or other public file, for the server to serve.
    [[nodiscard]] std::optional<std::filesystem::path> publicFile(const std::string& id, const std::string& path) const;

private:
    std::unique_ptr<IModuleInstance> load(const Manifest& m, const TrustedKey& signer,
                                          const std::filesystem::path& dir, const json& options, bool forceIsolated);
    /// reload() for callers that already hold adminMutex_.
    void reloadLocked();
    /// Shuts every loaded module down, in reverse dependency order, and forgets all entries.
    void shutdownAll();
    /// Shuts one module down, logging (not throwing) a failure.
    void shutdownLogged(IModuleInstance& instance) const;
    /// The module installed in `dir`, re-verified; nullopt (with a notice) if it fails verification.
    [[nodiscard]] std::optional<Entry> readInstalled(const std::filesystem::path& dir,
                                                     std::vector<std::string>& notices) const;
    /// Dependency-resolution state per module id: 0 = new, 1 = visiting, 2 = done.
    using VisitMarks = std::map<std::string, int, std::less<>>;
    void resolve();
    /// Resolves and loads one module after its dependencies; true if it ends up active. `lock`
    /// holds mutex_ and is released only while the module loads.
    bool visit(const std::string& id, VisitMarks& mark, std::unique_lock<std::mutex>& lock);
    /// False (with the state set) if the module is disabled or removed in this host version.
    bool mayLoad(Entry& e) const;
    /// Visits the required dependencies; false (state MissingDependency) at the first one that is not active.
    bool requiredDependenciesActive(Entry& e, VisitMarks& mark, std::unique_lock<std::mutex>& lock);
    /// Loads the module with `lock` released and records the outcome; true if it is active.
    bool start(const std::string& id, Entry& e, std::unique_lock<std::mutex>& lock);
    /// Deprecation and failure notices for one resolved module.
    void addNotices(const std::string& id, const Entry& e);
    /// The upload checks against what is installed: version upgrade, dependents, dependencies.
    void checkUpgrade(const Manifest& m, ValidationReport& report) const;
    void checkDependencies(const Manifest& m, ValidationReport& report) const;
    [[nodiscard]] std::string optionalDependencySummary(const Manifest& m) const;
    /// Unpacks into `staging` and runs the package's self-test in a worker.
    void runSelfTest(const VerifiedPackage& pkg, const std::filesystem::path& staging, ValidationReport& report);
    /// Moves a self-tested package from `staging` into place, keeping the old version's settings.
    void installStaged(const VerifiedPackage& pkg, const std::string& archive, const std::filesystem::path& staging,
                       ValidationReport& report);
    /// Takes the instance out of the entry, then shuts it down outside mutex_.
    void retire(Entry& e);
    void writeState(const Entry& e) const;
    [[nodiscard]] std::vector<std::string> dependentsOf(std::string_view id) const;
    void unpack(const PackageFiles& files, const std::filesystem::path& dir) const;

    RegistryConfig config_;
    ConfinedRoot root_;  ///< config_.root; every path the registry touches stays inside it
    const TrustStore& trust_;
    IHostServices& services_;
    /// Serialises the mutating calls. Module code (init, self-test, shutdown) runs while only this
    /// one is held, so a module that calls back into the host can still read the registry.
    std::mutex adminMutex_;
    /// Guards entries_, order_ and notices_: readers hold it to read, the admin caller to change them.
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::vector<std::string> order_;  ///< load order of active modules
    std::vector<std::string> notices_;
};

}  // namespace rfmod
