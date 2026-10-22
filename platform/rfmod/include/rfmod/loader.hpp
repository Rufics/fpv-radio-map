#pragma once

#include "rfmod/confined_root.hpp"
#include "rfmod/host.hpp"
#include "rfmod/trust.hpp"

#include <filesystem>
#include <memory>

namespace rfmod {

struct LoaderConfig {
    std::filesystem::path workerExecutable;  ///< rfmod-worker
    bool isolateAll = false;                 ///< Run first-party modules in workers too.
    ModulePolicy policy;                     ///< Ceilings for workers; requireSandbox binds third-party modules only.
};

/// Decides how a module runs (in process or in a worker) and starts it. The one place that knows
/// the policy: owner-signed modules may share the host process, every other module gets a worker
/// that has to confine itself.
class ModuleLoader {
public:
    ModuleLoader(LoaderConfig config, const ConfinedRoot& root, IHostServices& services);

    [[nodiscard]] bool isolates(const TrustedKey& signer) const;

    /// Starts the module from `dir` (inside the root). `forceIsolated` puts even an owner-signed
    /// module in a worker, which is how a package is self-tested before it is trusted.
    /// Throws ModuleError, PathEscapeError, json::exception or std::system_error.
    [[nodiscard]] std::shared_ptr<IModuleInstance> load(const Manifest& manifest, const TrustedKey& signer,
                                                        const std::filesystem::path& dir, const json& options,
                                                        bool forceIsolated) const;

    [[nodiscard]] const ModulePolicy& policy() const { return config_.policy; }

private:
    LoaderConfig config_;
    const ConfinedRoot& root_;
    IHostServices& services_;
};

}  // namespace rfmod
