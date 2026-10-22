#pragma once

#include "rfmod/dependency_resolver.hpp"
#include "rfmod/host.hpp"
#include "rfmod/installed.hpp"
#include "rfmod/loader.hpp"
#include "rfmod/module_store.hpp"
#include "rfmod/options_schema.hpp"
#include "rfmod/package.hpp"
#include "rfmod/trust.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

struct RegistryConfig {
    std::filesystem::path root;              ///< the module root, see ModuleStore
    std::filesystem::path workerExecutable;  ///< rfmod-worker
    Version hostVersion;
    bool isolateAll = false;  ///< Run first-party modules in workers too.
    PackageLimits limits;
    ModulePolicy policy;
};

/// The modules a running host has loaded from its module root. The registry never installs or
/// removes anything: the rfmod CLI does that on the machine, and reload() picks the result up.
/// What the admin API may change is whether a module is enabled and its options.
class ModuleRegistry {
public:
    struct Entry {
        InstalledModule installed;
        ModuleState state = ModuleState::Disabled;
        std::string stateDetail;
        std::shared_ptr<IModuleInstance> instance;
        std::shared_ptr<const OptionsSchema> optionsSchema;  ///< null when the manifest names none
    };

    ModuleRegistry(RegistryConfig config, const TrustStore& trust, IHostServices& services);
    ~ModuleRegistry();
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    /// Re-reads the module root, re-verifies every package against the current trust store,
    /// resolves dependencies and (re)loads modules in dependency order.
    void reload();
    /// Throws std::invalid_argument for an unknown module, or when disabling one another enabled module requires.
    void setEnabled(const std::string& id, bool enabled);
    /// Validates against the module's options schema; throws std::invalid_argument with the reason.
    void setOptions(const std::string& id, const json& options);
    /// Why `options` are not acceptable for the module (a request's per-module options), or
    /// nullopt when they are. An unknown module is a reason too.
    [[nodiscard]] std::optional<std::string> checkOptions(const std::string& id, const json& options) const;

    /// One object per installed module: id, version, title, lifecycle, provides, signedBy,
    /// isolated, state[, stateDetail], requires, optional[, deprecation][, ui][, options].
    /// `ui.bundle` is the package-relative path; the server turns it into a URL.
    [[nodiscard]] json list() const;
    [[nodiscard]] std::vector<std::string> notices() const;
    /// Active modules providing a capability, in dependency order. The instances stay usable
    /// after a reload: a call then fails with ModuleError "unavailable".
    [[nodiscard]] std::vector<std::shared_ptr<IModuleInstance>> withCapability(const std::string& capability) const;
    [[nodiscard]] std::shared_ptr<IModuleInstance> instance(const std::string& id) const;

    /// A module's UI bundle or options schema file, for the server to serve; never lib/ and
    /// never outside the module's directory.
    [[nodiscard]] std::optional<std::filesystem::path> publicFile(const std::string& id, const std::string& path) const;

private:
    using Entries = std::map<std::string, Entry, std::less<>>;

    /// reload() for callers that already hold adminMutex_.
    void reloadLocked();
    /// Shuts every loaded module down, in reverse dependency order, and forgets all entries.
    void shutdownAll();
    void shutdownLogged(IModuleInstance& instance) const;
    /// The entries for what is installed, with their options schemas compiled.
    [[nodiscard]] Entries readEntries(std::vector<std::string>& notices) const;
    /// Loads the planned modules in order, recording each outcome.
    void loadAll(const std::vector<DependencyResolver::Step>& plan);
    /// Loads one module whose dependencies are active; true if it is.
    bool loadOne(Entry& e);
    [[nodiscard]] bool requiredDependenciesActive(const Entry& e, std::string& detail) const;
    void addNotices(const std::string& id, const Entry& e);
    /// The stored settings with the change applied; writes them and reloads.
    void updateSettings(const std::string& id, const ModuleSettings& settings);
    [[nodiscard]] std::vector<std::string> dependentsOf(std::string_view id) const;
    [[nodiscard]] json describe(const Entry& e) const;

    RegistryConfig config_;
    ModuleStore store_;
    const TrustStore& trust_;
    IHostServices& services_;
    PackageVerifier verifier_;
    ModuleLoader loader_;
    /// Serialises the mutating calls. Module code (init, shutdown) runs while only this one is
    /// held, so a module that calls back into the host can still read the registry.
    std::mutex adminMutex_;
    /// Guards entries_, order_ and notices_: readers hold it to read, the admin caller to change them.
    mutable std::mutex mutex_;
    Entries entries_;
    std::vector<std::string> order_;  ///< load order of active modules
    std::vector<std::string> notices_;
};

}  // namespace rfmod
