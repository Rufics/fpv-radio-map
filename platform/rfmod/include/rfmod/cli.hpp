#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

namespace rfmod {

class CliArgs;
struct InstallerConfig;
class TrustStore;
class ValidationReport;
class Version;

/// What the build gives the rfmod tool: the host version its packages are checked against, the
/// owner key the operator commands trust, and the rfmod-worker that runs self-tests.
struct CliBuild {
    std::string hostVersion;
    std::string ownerKey;  ///< empty when the build has none: the operator commands then refuse to run
    std::filesystem::path workerExecutable;
};

/// The rfmod tool: the module developer's and the operator's commands, one method each.
///
/// Developer commands:
///   rfmod keygen --out NAME              write NAME.pub and NAME.secret (keep the secret offline)
///   rfmod new ID [--dir DIR]             scaffold a module project
///   rfmod pack DIR --key NAME.secret [-o FILE.rfmod]
///   rfmod inspect FILE.rfmod
///   rfmod check FILE.rfmod --trust KEY.pub [--trust ...] [--host-version X.Y.Z]
///   rfmod test FILE.rfmod --trust KEY.pub [--with DEP.rfmod ...] [--fixtures FILE.json]
///
/// Operator commands, run on the server machine against its module root. They are the only way a
/// module gets onto a server: the HTTP API lists modules and toggles them, it never installs.
///   rfmod install FILE.rfmod --root DIR --trust-store KEYS.json [--fixtures FILE.json]
///   rfmod remove ID --root DIR --trust-store KEYS.json
///   rfmod list --root DIR --trust-store KEYS.json
///   rfmod trust list|add KEY.pub --label TEXT|remove KEY_ID  --trust-store KEYS.json
///
/// "check" runs every install check except the self-test; "test" and "install" also run the
/// self-test in a worker exactly as the server would. Host calls are answered from --fixtures
/// ({"method": response, ...}); anything else fails, so self-tests should not depend on them.
/// The operator commands trust the owner key this build was made with plus the keys in the trust
/// store file; the developer commands trust only the --trust keys, the first of which plays the
/// owner's part.
class Cli {
public:
    /// Results go to `out`, usage and errors to `err`.
    Cli(CliBuild build, std::ostream& out, std::ostream& err);

    /// Runs the command line (without the program name) and returns the exit status: 0 when the
    /// command succeeded, 1 when it failed or a check did not pass, 64 for a usage error.
    [[nodiscard]] int run(std::span<const std::string> words) const;

    static void usage(std::ostream& err);

private:
    [[nodiscard]] int dispatch(const std::string& command, const CliArgs& args) const;
    [[nodiscard]] int printReport(const ValidationReport& r) const;
    [[nodiscard]] Version hostVersion(const CliArgs& args) const;
    [[nodiscard]] TrustStore operatorTrust(const CliArgs& args) const;
    [[nodiscard]] InstallerConfig installerConfig(const CliArgs& args, const std::filesystem::path& root) const;

    [[nodiscard]] int keygen(const CliArgs& args) const;
    [[nodiscard]] int scaffold(const CliArgs& args) const;
    [[nodiscard]] int pack(const CliArgs& args) const;
    [[nodiscard]] int inspect(const CliArgs& args) const;
    [[nodiscard]] int check(const CliArgs& args) const;
    [[nodiscard]] int test(const CliArgs& args) const;
    [[nodiscard]] int install(const CliArgs& args) const;
    [[nodiscard]] int remove(const CliArgs& args) const;
    [[nodiscard]] int list(const CliArgs& args) const;
    [[nodiscard]] int trust(const CliArgs& args) const;

    CliBuild build_;
    std::ostream& out_;
    std::ostream& err_;
};

}  // namespace rfmod
