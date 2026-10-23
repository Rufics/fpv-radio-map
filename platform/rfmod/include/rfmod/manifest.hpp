#pragma once

#include "rfmod/abi.h"
#include "rfmod/report.hpp"
#include "rfmod/semver.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

enum class Lifecycle { Alpha, Beta, Stable, Deprecated };

std::string toString(Lifecycle l);

struct Dependency {
    std::string id;
    VersionRange range;
};

struct Deprecation {
    Version since;
    Version removedIn;
    std::string replacement;
    std::string message;
};

struct UiExtension {
    std::string bundle;
    std::vector<std::string> slots;
};

struct Limits {
    unsigned memoryMb = 512;
    unsigned timeoutMs = 30000;
};

/// A parsed, validated rfmod/v1 manifest.
struct Manifest {
    std::string id;
    Version version;
    std::string title;
    std::string description;
    std::string author;
    std::string license;
    Lifecycle lifecycle = Lifecycle::Alpha;
    std::optional<Deprecation> deprecation;
    unsigned abi = RFMOD_ABI_VERSION;
    std::optional<VersionRange> hostRange;
    std::vector<Dependency> dependencies;          ///< manifest "requires"
    std::vector<Dependency> optionalDependencies;  ///< manifest "optional"
    std::vector<std::string> provides;
    std::vector<std::string> methods;
    std::map<std::string, std::string, std::less<>> entry;  ///< platform -> lib/... path
    std::optional<UiExtension> ui;
    std::optional<std::string> optionsSchema;
    Limits limits;
    nlohmann::json raw;

    [[nodiscard]] bool hasCapability(std::string_view capability) const;
};

/// The platform key of this build, for example "linux-x86_64".
std::string currentPlatform();

/// Reads manifest.json for one host version: schema validation (rfmod-manifest.schema.json,
/// embedded at build time) plus the rules a schema cannot express.
class ManifestParser {
public:
    explicit ManifestParser(Version hostVersion) : hostVersion_(std::move(hostVersion)) {}

    /// Adds its checks to `report`; returns the manifest only when all pass.
    [[nodiscard]] std::optional<Manifest> parse(const nlohmann::json& document, ValidationReport& report) const;

    /// Minimum distance between deprecation and removal, Kubernetes style: 3 minor releases.
    static constexpr unsigned kMinDeprecationMinors = 3;

private:
    /// Versions, ranges and dependency lists; false (with the check failed) when one does not parse.
    static bool readVersions(const nlohmann::json& document, Manifest& m, ValidationReport& report);
    /// Dependency ids are unique and not the module itself; adds the check, returns whether it passed.
    static bool checkDependencies(const Manifest& m, ValidationReport& report);
    /// Reads the deprecation block into `m` and checks its window and lifecycle.
    static bool checkDeprecation(const nlohmann::json& d, Manifest& m, ValidationReport& report);
    /// The optional blocks: ui, optionsSchema, limits.
    static bool readExtensions(const nlohmann::json& document, Manifest& m, ValidationReport& report);

    Version hostVersion_;
};

}  // namespace rfmod
