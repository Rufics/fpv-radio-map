#pragma once

#include "rfmod/report.hpp"
#include "rfmod/semver.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
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
    unsigned abi = 1;
    std::optional<VersionRange> hostRange;
    std::vector<Dependency> dependencies;          ///< manifest "requires"
    std::vector<Dependency> optionalDependencies;  ///< manifest "optional"
    std::vector<std::string> provides;
    std::vector<std::string> methods;
    std::map<std::string, std::string> entry;  ///< platform -> lib/... path
    std::optional<UiExtension> ui;
    std::optional<std::string> optionsSchema;
    Limits limits;
    nlohmann::json raw;

    [[nodiscard]] bool hasCapability(const std::string& capability) const;
};

/// The platform key of this build, for example "linux-x86_64".
std::string currentPlatform();

class ManifestParser {
public:
    /// Schema validation (rfmod-manifest.schema.json, embedded at build time) plus the rules a
    /// schema cannot express. Adds its checks to `report`; returns the manifest only when all pass.
    static std::optional<Manifest> parse(const nlohmann::json& document, const Version& hostVersion,
                                         ValidationReport& report);

    /// Minimum distance between deprecation and removal, Kubernetes style: 3 minor releases.
    static constexpr unsigned kMinDeprecationMinors = 3;
};

}  // namespace rfmod
