#pragma once

#include "rfmod/host.hpp"
#include "rfmod/installed.hpp"
#include "rfmod/loader.hpp"
#include "rfmod/module_store.hpp"
#include "rfmod/package.hpp"
#include "rfmod/report.hpp"
#include "rfmod/trust.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

struct InstallerConfig {
    std::filesystem::path root;              ///< the module root, see ModuleStore
    std::filesystem::path workerExecutable;  ///< rfmod-worker, for the self-test
    Version hostVersion;
    PackageLimits limits;
    ModulePolicy policy;
};

/// Puts packages into a module root and takes them out again: the rfmod CLI's job, never the
/// server's. Every package is verified, checked against what is installed, self-tested in a
/// worker and only then moved into place. The server picks the result up with
/// ModuleRegistry::reload().
class ModuleInstaller {
public:
    /// `selfTestHost` answers the host calls a self-test makes.
    ModuleInstaller(InstallerConfig config, const TrustStore& trust, IHostServices& selfTestHost);

    /// Every check an install runs, without installing.
    [[nodiscard]] ValidationReport check(std::string_view archive) const;
    /// Installs when every check passes. Replaces an older version of the same module, keeping
    /// its settings when they still fit the new options schema.
    [[nodiscard]] ValidationReport install(std::string_view archive) const;
    /// Throws std::invalid_argument when the module is unknown or another enabled module requires it.
    void remove(const std::string& id) const;

    /// The modules in the store whose packages still verify, by id.
    [[nodiscard]] std::map<std::string, InstalledModule, std::less<>> installed() const;

    /// Module ids under these prefixes belong to the platform: only the owner key may sign them.
    static constexpr std::array<std::string_view, 2> kReservedPrefixes = {"fpv.", "rf."};
    [[nodiscard]] static bool reservedId(std::string_view id);
    /// True for a string of the manifest id alphabet: what a directory under installed/ is named.
    [[nodiscard]] static bool looksLikeModuleId(std::string_view id);

private:
    using Installed = std::map<std::string, InstalledModule, std::less<>>;

    [[nodiscard]] ValidationReport run(std::string_view archive, bool install) const;
    /// The checks against what is installed: signer, version upgrade, dependents, dependencies, lifecycle.
    void checkAgainstInstalled(const VerifiedPackage& pkg, const Installed& have, ValidationReport& report) const;
    void checkSigner(const VerifiedPackage& pkg, const Installed& have, ValidationReport& report) const;
    void checkUpgrade(const Manifest& m, const Installed& have, ValidationReport& report) const;
    void checkDependencies(const Manifest& m, const Installed& have, ValidationReport& report) const;
    void checkLifecycle(const Manifest& m, ValidationReport& report) const;
    [[nodiscard]] static std::string optionalDependencySummary(const Manifest& m, const Installed& have);
    /// Unpacks into `staging` and runs the package's self-test in a worker.
    void runSelfTest(const VerifiedPackage& pkg, const std::filesystem::path& staging, ValidationReport& report) const;
    /// Moves a self-tested package from `staging` into place, keeping the old version's settings.
    void installStaged(const VerifiedPackage& pkg, std::string_view archive, const std::filesystem::path& staging,
                       const Installed& have, ValidationReport& report) const;
    [[nodiscard]] static std::vector<std::string> dependentsOf(std::string_view id, const Installed& have);

    InstallerConfig config_;
    ModuleStore store_;
    const TrustStore& trust_;
    PackageVerifier verifier_;
    ModuleLoader loader_;
};

}  // namespace rfmod
