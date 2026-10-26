#include "rfmod/manifest.hpp"

#include "manifest_schema.hpp"  // generated: kManifestSchema

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

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

Lifecycle parseLifecycle(std::string_view s) {
    using enum Lifecycle;
    if (s == "alpha") return Alpha;
    if (s == "beta") return Beta;
    if (s == "stable") return Stable;
    return Deprecated;
}

std::string join(const std::vector<std::string>& items) {
    std::ostringstream out;
    for (std::size_t i = 0; i < items.size(); ++i) out << (i ? "; " : "") << items[i];
    return out.str();
}

}  // namespace

std::string toString(Lifecycle l) {
    using enum Lifecycle;
    switch (l) {
        case Alpha: return "alpha";
        case Beta: return "beta";
        case Stable: return "stable";
        case Deprecated: return "deprecated";
    }
    return "alpha";
}

bool Manifest::hasCapability(std::string_view capability) const {
    return std::ranges::find(provides, capability) != provides.end();
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

bool ManifestParser::checkDependencies(const Manifest& m, ValidationReport& report) {
    std::set<std::string, std::less<>> seen;
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
    report.add("manifest.dependencies", depsOk, depDetail);
    return depsOk;
}

bool ManifestParser::checkDeprecation(const json& d, Manifest& m, ValidationReport& report) {
    try {
        Deprecation dep{Version::parse(d.at("since").get<std::string>()),
                        Version::parse(d.at("removedIn").get<std::string>()), d.value("replacement", ""),
                        d.at("message").get<std::string>()};
        // Kubernetes-style window: removal at least 3 minor releases after deprecation, or a later major.
        // Subtracting (not adding) keeps the comparison inside unsigned range for any version.
        const bool windowOk = dep.removedIn.major() > dep.since.major() ||
                              (dep.removedIn.major() == dep.since.major() &&
                               dep.removedIn.minor() >= kMinDeprecationMinors &&
                               dep.removedIn.minor() - kMinDeprecationMinors >= dep.since.minor());
        report.add("deprecation.window", windowOk,
                   std::format("deprecated in {}, removed in {} (needs at least {} minor releases)", dep.since.str(),
                               dep.removedIn.str(), kMinDeprecationMinors));
        const bool lifecycleOk = m.lifecycle == Lifecycle::Deprecated;
        report.add("deprecation.lifecycle", lifecycleOk, "a deprecation block requires lifecycle \"deprecated\"");
        m.deprecation = std::move(dep);
        return windowOk && lifecycleOk;
    } catch (const std::invalid_argument& e) {
        report.add("deprecation.window", false, e.what());
    } catch (const json::exception& e) {
        report.add("deprecation.window", false, e.what());
    }
    return false;
}

bool ManifestParser::readVersions(const json& document, Manifest& m, ValidationReport& report) {
    // Versions and ranges: the schema checks shape, the parser checks meaning.
    try {
        m.version = Version::parse(document.at("version").get<std::string>());
        if (document.contains("host")) m.hostRange = VersionRange::parse(document.at("host").get<std::string>());
        auto deps = [](const json& list) {
            std::vector<Dependency> out;
            for (const json& d : list) {
                out.emplace_back(d.at("id").get<std::string>(), VersionRange::parse(d.at("version").get<std::string>()));
            }
            return out;
        };
        m.dependencies = deps(document.at("requires"));
        m.optionalDependencies = deps(document.value("optional", json::array()));
        report.pass("manifest.versions", "version " + m.version.str() + " and all ranges parse");
        return true;
    } catch (const std::invalid_argument& e) {
        report.fail("manifest.versions", e.what());
    } catch (const json::exception& e) {
        report.fail("manifest.versions", e.what());
    }
    return false;
}

bool ManifestParser::readExtensions(const json& document, Manifest& m, ValidationReport& report) {
    bool ok = true;
    if (document.contains("ui")) {
        m.ui = UiExtension{document.at("ui").at("bundle").get<std::string>(),
                           document.at("ui").at("slots").get<std::vector<std::string>>()};
        ok = m.hasCapability("ui.panel");
        report.add("ui.capability", ok, "a ui block requires provides: ui.panel");
    }
    if (document.contains("optionsSchema")) m.optionsSchema = document.at("optionsSchema").get<std::string>();
    if (document.contains("limits")) {
        const json& l = document.at("limits");
        m.limits.memoryMb = l.value("memoryMb", m.limits.memoryMb);
        m.limits.timeoutMs = l.value("timeoutMs", m.limits.timeoutMs);
    }
    return ok;
}

std::optional<Manifest> ManifestParser::parse(const json& document, ValidationReport& report) const {
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
    m.entry = document.at("entry").get<std::map<std::string, std::string, std::less<>>>();
    if (!readVersions(document, m, report)) return std::nullopt;

    bool ok = true;
    auto check = [&report, &ok](const std::string& name, bool passed, const std::string& detail) {
        report.add(name, passed, detail);
        ok = ok && passed;
    };
    check("abi", m.abi == RFMOD_ABI_VERSION,
          std::format("module ABI {}, host supports {}", m.abi, RFMOD_ABI_VERSION));
    if (m.hostRange) {
        check("host.version", m.hostRange->contains(hostVersion_),
              "module needs host " + m.hostRange->str() + ", this host is " + hostVersion_.str());
    }
    const std::string platform = currentPlatform();
    check("entry.platform", m.entry.contains(platform),
          m.entry.contains(platform) ? "library for " + platform + ": " + m.entry.at(platform)
                                     : "no library for " + platform + " in entry");
    ok = checkDependencies(m, report) && ok;
    if (document.contains("deprecation")) ok = checkDeprecation(document.at("deprecation"), m, report) && ok;
    ok = readExtensions(document, m, report) && ok;

    if (!ok) return std::nullopt;
    return m;
}

}  // namespace rfmod
