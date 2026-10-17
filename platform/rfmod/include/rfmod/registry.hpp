#pragma once

#include "rfmod/host.hpp"
#include "rfmod/package.hpp"
#include "rfmod/trust.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rfmod {

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
    void resolve();
    void writeState(const Entry& e) const;
    [[nodiscard]] std::vector<std::string> dependentsOf(const std::string& id) const;
    static void unpack(const PackageFiles& files, const std::filesystem::path& dir);

    RegistryConfig config_;
    const TrustStore& trust_;
    IHostServices& services_;
    mutable std::recursive_mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::vector<std::string> order_;  ///< load order of active modules
    std::vector<std::string> notices_;
};

}  // namespace rfmod
