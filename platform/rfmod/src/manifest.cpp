#include "rfmod/manifest.hpp"

#include "manifest_schema.hpp"  // generated: kManifestSchema

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <sstream>

namespace rfmod {
namespace {

using json = nlohmann::json;

class CollectingErrors : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const json::json_pointer& ptr, const json& instance, const std::string& message) override {
        basic_error_handler::error(ptr, instance, message);
        std::string where = ptr.to_string();
        lines.push_back((where.empty() ? std::string("/") : where) + ": " + message);
    }
    std::vector<std::string> lines;
};

const nlohmann::json_schema::json_validator& schemaValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        nlohmann::json_schema::json_validator v(nullptr, nlohmann::json_schema::default_string_format_check);
        v.set_root_schema(json::parse(kManifestSchema));
        return v;
    }();
    return validator;
}

Lifecycle parseLifecycle(const std::string& s) {
    if (s == "alpha") return Lifecycle::Alpha;
    if (s == "beta") return Lifecycle::Beta;
    if (s == "stable") return Lifecycle::Stable;
    return Lifecycle::Deprecated;
}

std::string join(const std::vector<std::string>& items) {
    std::ostringstream out;
    for (std::size_t i = 0; i < items.size(); ++i) out << (i ? "; " : "") << items[i];
    return out.str();
}

}  // namespace

std::string toString(Lifecycle l) {
    switch (l) {
        case Lifecycle::Alpha: return "alpha";
        case Lifecycle::Beta: return "beta";
        case Lifecycle::Stable: return "stable";
        case Lifecycle::Deprecated: return "deprecated";
    }
    return "alpha";
}

bool Manifest::hasCapability(const std::string& capability) const {
    return std::find(provides.begin(), provides.end(), capability) != provides.end();
}

std::string currentPlatform() {
#if defined(__linux__)
    const char* os = "linux";
#elif defined(__APPLE__)
    const char* os = "macos";
#elif defined(_WIN32)
    const char* os = "windows";
#else
    const char* os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "aarch64";
#else
    const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

std::optional<Manifest> ManifestParser::parse(const json& document, const Version& hostVersion,
                                              ValidationReport& report) {
    CollectingErrors errors;
    schemaValidator().validate(document, errors);
    if (errors) {
        report.fail("manifest.schema", join(errors.lines));
        return std::nullopt;
    }
    report.pass("manifest.schema", "manifest.json matches rfmod/v1");

    Manifest m;
    m.raw = document;
    m.id = document.at("id").get<std::string>();
    m.title = document.at("title").get<std::string>();
    m.description = document.value("description", "");
    m.author = document.value("author", "");
    m.license = document.value("license", "");
    m.lifecycle = parseLifecycle(document.at("lifecycle").get<std::string>());
    m.abi = document.at("abi").get<unsigned>();
    m.provides = document.at("provides").get<std::vector<std::string>>();
    m.methods = document.value("methods", std::vector<std::string>{});
    m.entry = document.at("entry").get<std::map<std::string, std::string>>();

    bool ok = true;
    auto check = [&](const std::string& name, bool passed, const std::string& detail) {
        report.add(name, passed, detail);
        ok = ok && passed;
    };

    // Versions and ranges: the schema checks shape, the parser checks meaning.
    try {
        m.version = Version::parse(document.at("version").get<std::string>());
        if (document.contains("host")) m.hostRange = VersionRange::parse(document.at("host").get<std::string>());
        auto deps = [](const json& list) {
            std::vector<Dependency> out;
            for (const json& d : list) {
                out.push_back({d.at("id").get<std::string>(), VersionRange::parse(d.at("version").get<std::string>())});
            }
            return out;
        };
        m.dependencies = deps(document.at("requires"));
        m.optionalDependencies = deps(document.value("optional", json::array()));
        report.pass("manifest.versions", "version " + m.version.str() + " and all ranges parse");
    } catch (const std::exception& e) {
        check("manifest.versions", false, e.what());
        return std::nullopt;
    }

    check("abi", m.abi == RFMOD_ABI_VERSION,
          std::format("module ABI {}, host supports {}", m.abi, RFMOD_ABI_VERSION));

    if (m.hostRange) {
        check("host.version", m.hostRange->contains(hostVersion),
              "module needs host " + m.hostRange->str() + ", this host is " + hostVersion.str());
    }

    const std::string platform = currentPlatform();
    check("entry.platform", m.entry.count(platform) == 1,
          m.entry.count(platform) ? "library for " + platform + ": " + m.entry.at(platform)
                                  : "no library for " + platform + " in entry");

    std::set<std::string> seen;
    bool depsOk = true;
    std::string depDetail = "dependency ids are unique and not the module itself";
    for (const auto* list : {&m.dependencies, &m.optionalDependencies}) {
        for (const Dependency& d : *list) {
            if (d.id == m.id || !seen.insert(d.id).second) {
                depsOk = false;
                depDetail = "dependency " + d.id + " is listed twice or is the module itself";
            }
        }
    }
    check("manifest.dependencies", depsOk, depDetail);

    if (document.contains("deprecation")) {
        const json& d = document.at("deprecation");
        try {
            Deprecation dep{Version::parse(d.at("since").get<std::string>()),
                            Version::parse(d.at("removedIn").get<std::string>()), d.value("replacement", ""),
                            d.at("message").get<std::string>()};
            // Kubernetes-style window: removal at least 3 minor releases after deprecation, or a later major.
            const bool windowOk = dep.removedIn.major() > dep.since.major() ||
                                  (dep.removedIn.major() == dep.since.major() &&
                                   dep.removedIn.minor() >= dep.since.minor() + kMinDeprecationMinors);
            check("deprecation.window", windowOk,
                  "deprecated in " + dep.since.str() + ", removed in " + dep.removedIn.str() +
                      " (needs at least " + std::to_string(kMinDeprecationMinors) + " minor releases)");
            check("deprecation.lifecycle", m.lifecycle == Lifecycle::Deprecated,
                  "a deprecation block requires lifecycle \"deprecated\"");
            m.deprecation = std::move(dep);
        } catch (const std::exception& e) {
            check("deprecation.window", false, e.what());
        }
    }

    if (document.contains("ui")) {
        m.ui = UiExtension{document.at("ui").at("bundle").get<std::string>(),
                           document.at("ui").at("slots").get<std::vector<std::string>>()};
        check("ui.capability", m.hasCapability("ui.panel"), "a ui block requires provides: ui.panel");
    }
    if (document.contains("optionsSchema")) m.optionsSchema = document.at("optionsSchema").get<std::string>();
    if (document.contains("limits")) {
        const json& l = document.at("limits");
        m.limits.memoryMb = l.value("memoryMb", m.limits.memoryMb);
        m.limits.timeoutMs = l.value("timeoutMs", m.limits.timeoutMs);
    }

    if (!ok) return std::nullopt;
    return m;
}

}  // namespace rfmod
