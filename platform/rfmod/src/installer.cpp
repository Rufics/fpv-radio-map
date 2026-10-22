#include "rfmod/installer.hpp"

#include "rfmod/options_schema.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <system_error>

namespace rfmod {
namespace fs = std::filesystem;

namespace {

std::string sandboxDetail(const IsolatedModule& isolated) {
    return std::format("separate process, memory limit {} MB, timeout {} ms; {}", isolated.limits().memoryMb,
                       isolated.limits().timeoutMs, isolated.sandbox().describe());
}

std::string selfTestDetail(const json& result, bool passed) {
    std::string detail = passed ? "self-test passed" : "self-test failed";
    if (result.contains("checks")) {
        for (const json& c : result.at("checks")) {
            detail += "; " + c.value("name", "?") + (c.value("ok", false) ? " ok" : " FAILED");
        }
    }
    return detail;
}

std::string joined(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : sep) + item;
    return out;
}

/// Whether the old settings still match the new version's options schema.
bool optionsStillFit(const ModuleStore& store, const fs::path& dir, const Manifest& m, const json& options) {
    if (!m.optionsSchema) return options.is_object() && options.empty();
    try {
        return !OptionsSchema(json::parse(store.readFile(dir / *m.optionsSchema))).check(options).has_value();
    } catch (const std::invalid_argument&) {  // the new schema cannot be used
        return false;
    } catch (const json::exception&) {  // the new schema is not JSON
        return false;
    }
}

}  // namespace

ModuleInstaller::ModuleInstaller(InstallerConfig config, const TrustStore& trust, IHostServices& selfTestHost)
    : config_(std::move(config)),
      store_(config_.root),
      trust_(trust),
      verifier_(trust_, config_.hostVersion, config_.limits),
      loader_(LoaderConfig{config_.workerExecutable, true, config_.policy}, store_.root(), selfTestHost) {
    // Staging directories are the installer's; whatever an earlier run left behind is garbage.
    store_.clearStaging();
}

bool ModuleInstaller::reservedId(std::string_view id) {
    return std::ranges::any_of(kReservedPrefixes, [id](std::string_view prefix) { return id.starts_with(prefix); });
}

std::map<std::string, InstalledModule, std::less<>> ModuleInstaller::installed() const {
    std::vector<std::string> notices;
    return InstalledReader(store_, verifier_).all(notices);
}

ValidationReport ModuleInstaller::check(std::string_view archive) const { return run(archive, false); }

ValidationReport ModuleInstaller::install(std::string_view archive) const { return run(archive, true); }

ValidationReport ModuleInstaller::run(std::string_view archive, bool install) const {
    ValidationReport report;
    auto pkg = verifier_.verify(archive, report);
    if (!pkg) return report;

    const Installed have = installed();
    checkAgainstInstalled(*pkg, have, report);
    if (!report.ok()) return report;

    // Self-test always runs in a worker, whoever signed it: a package is untrusted until installed.
    const fs::path staging = store_.newStagingDir();
    runSelfTest(*pkg, staging, report);
    if (!report.ok() || !install) {
        store_.remove(staging);
        return report;
    }
    installStaged(*pkg, archive, staging, have, report);
    return report;
}

void ModuleInstaller::checkAgainstInstalled(const VerifiedPackage& pkg, const Installed& have,
                                            ValidationReport& report) const {
    checkSigner(pkg, have, report);
    checkUpgrade(pkg.manifest, have, report);
    checkDependencies(pkg.manifest, have, report);
    checkLifecycle(pkg.manifest, report);
}

void ModuleInstaller::checkSigner(const VerifiedPackage& pkg, const Installed& have, ValidationReport& report) const {
    const std::string& id = pkg.manifest.id;
    if (reservedId(id) && !pkg.signer.firstParty()) {
        report.fail("signer.reserved", id + " is a platform module id; only the owner key may sign it");
        return;
    }
    // A module stays with the key that first installed it: another vendor's key cannot take over
    // an id, which would hand it the dependents and the options of the module it replaces.
    const auto existing = have.find(id);
    if (existing == have.end() || pkg.signer.firstParty() || existing->second.signer.key.id() == pkg.signer.key.id()) {
        report.pass("signer", pkg.signer.label + " (" + pkg.signer.key.id() + ")");
        return;
    }
    report.fail("signer", id + " is installed from key " + existing->second.signer.key.id() + " (" +
                              existing->second.signer.label + "); a package from " + pkg.signer.key.id() +
                              " cannot replace it");
}

void ModuleInstaller::checkUpgrade(const Manifest& m, const Installed& have, ValidationReport& report) const {
    if (const auto existing = have.find(m.id); existing != have.end()) {
        const Version& current = existing->second.manifest.version;
        report.add("version.upgrade", m.version > current,
                   m.version > current ? "upgrades " + current.str() + " to " + m.version.str()
                                       : "version " + m.version.str() + " is not newer than installed " + current.str());
    }
    // An upgrade must not leave an enabled dependent outside the range it asked for.
    std::vector<std::string> stranded;
    for (const auto& [otherId, other] : have) {
        if (!other.settings.enabled || otherId == m.id) continue;
        for (const Dependency& d : other.manifest.dependencies) {
            if (d.id == m.id && !d.range.contains(m.version)) stranded.push_back(otherId + " needs " + d.range.str());
        }
    }
    if (!stranded.empty()) report.fail("dependents", m.version.str() + " breaks enabled modules: " + joined(stranded, "; "));
}

std::string ModuleInstaller::optionalDependencySummary(const Manifest& m, const Installed& have) {
    std::vector<std::string> parts;
    for (const Dependency& d : m.optionalDependencies) {
        const auto it = have.find(d.id);
        const bool ok = it != have.end() && d.range.contains(it->second.manifest.version);
        parts.push_back(d.id + (ok ? " present" : " absent, features off"));
    }
    return joined(parts, "; ");
}

void ModuleInstaller::checkDependencies(const Manifest& m, const Installed& have, ValidationReport& report) const {
    // Required dependencies must already be installed, in range and enabled.
    std::vector<std::string> problems;
    for (const Dependency& d : m.dependencies) {
        const auto it = have.find(d.id);
        if (it == have.end()) {
            problems.push_back(d.id + " " + d.range.str() + " is not installed");
        } else if (!d.range.contains(it->second.manifest.version)) {
            problems.push_back(d.id + " " + d.range.str() + " needed, " + it->second.manifest.version.str() + " installed");
        } else if (!it->second.settings.enabled) {
            problems.push_back(d.id + " is disabled");
        } else if (const auto& dep = it->second.manifest.deprecation;
                   it->second.manifest.lifecycle == Lifecycle::Deprecated && dep.has_value()) {
            report.pass("dependencies.deprecated", d.id + " is deprecated: " + dep->message);
        }
    }
    if (!problems.empty()) {
        report.fail("dependencies", joined(problems, "; "));
        return;
    }
    std::string detail = "no required dependencies";
    if (!m.dependencies.empty()) {
        detail = std::format("{} required, all installed", m.dependencies.size());
        if (const std::string optional = optionalDependencySummary(m, have); !optional.empty()) {
            detail += "; optional: " + optional;
        }
    }
    report.pass("dependencies", detail);
}

void ModuleInstaller::checkLifecycle(const Manifest& m, ValidationReport& report) const {
    if (m.lifecycle != Lifecycle::Deprecated || !m.deprecation.has_value()) {
        report.pass("lifecycle", toString(m.lifecycle));
        return;
    }
    const Deprecation& d = *m.deprecation;
    report.pass("lifecycle", "deprecated since " + d.since.str() + ", removed in " + d.removedIn.str() + ": " + d.message);
    if (config_.hostVersion >= d.removedIn) {
        report.fail("lifecycle.removed", "this host (" + config_.hostVersion.str() + ") no longer accepts it");
    }
}

void ModuleInstaller::runSelfTest(const VerifiedPackage& pkg, const fs::path& staging, ValidationReport& report) const {
    try {
        store_.unpack(pkg.files, staging);
        const std::shared_ptr<IModuleInstance> instance = loader_.load(pkg.manifest, pkg.signer, staging, json::object(), true);
        if (const auto* iso = dynamic_cast<const IsolatedModule*>(instance.get())) {
            // Informational: says what this machine's kernel allowed, so nobody assumes more.
            report.pass("sandbox", sandboxDetail(*iso));
        }
        const json result = instance->invoke("rfmod.selftest", json::object());
        instance->shutdown();
        const bool passed = result.value("ok", false);
        report.add("selftest", passed, selfTestDetail(result, passed));
    } catch (const ModuleError& e) {
        report.fail("selftest", e.code() + ": " + e.what());
    } catch (const json::exception& e) {  // a malformed self-test reply
        report.fail("selftest", e.what());
    } catch (const PathEscapeError& e) {  // a package path that leaves the staging directory
        report.fail("selftest", e.what());
    } catch (const RegistryWriteError& e) {
        report.fail("selftest", e.what());
    } catch (const std::system_error& e) {  // unpacking failed, or no worker could be started
        report.fail("selftest", e.what());
    }
}

void ModuleInstaller::installStaged(const VerifiedPackage& pkg, std::string_view archive, const fs::path& staging,
                                    const Installed& have, ValidationReport& report) const {
    const Manifest& m = pkg.manifest;
    store_.writePackage(staging, archive);
    ModuleSettings settings;
    if (const auto existing = have.find(m.id); existing != have.end()) {
        settings.enabled = existing->second.settings.enabled;
        if (optionsStillFit(store_, staging, m, existing->second.settings.options)) {
            settings.options = existing->second.settings.options;
        }
    }
    store_.writeSettings(staging, settings, m, pkg.signer);
    store_.replace(staging, m.id);
    report.pass("installed", m.id + " " + m.version.str() + " in " + store_.installedDir(m.id).string());
}

bool ModuleInstaller::looksLikeModuleId(std::string_view id) {
    // The manifest id shape (schemas/rfmod-manifest.schema.json): lower-case words of letters,
    // digits and dashes, each starting with a letter, joined by single dots. Never "." or "..",
    // never a path.
    if (id.empty() || id.size() > 128) return false;
    std::size_t words = 0;
    for (const auto word : id | std::views::split('.')) {
        const std::string_view w(word.begin(), word.end());
        if (w.empty() || !std::islower(static_cast<unsigned char>(w.front()))) return false;
        const bool plain = std::ranges::all_of(w, [](char c) { return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-'; });
        if (!plain) return false;
        ++words;
    }
    return words >= 2;
}

std::vector<std::string> ModuleInstaller::dependentsOf(std::string_view id, const Installed& have) {
    std::vector<std::string> out;
    for (const auto& [otherId, m] : have) {
        if (!m.settings.enabled) continue;
        for (const Dependency& d : m.manifest.dependencies) {
            if (d.id == id) out.push_back(otherId);
        }
    }
    return out;
}

void ModuleInstaller::remove(const std::string& id) const {
    const Installed have = installed();
    const auto it = have.find(id);
    if (it == have.end()) {
        // A directory whose package no longer verifies is still removable, by its directory name,
        // which must look like a module id: never a path, never the installed root itself.
        if (!looksLikeModuleId(id) || !store_.isInstalled(id)) throw std::invalid_argument("unknown module " + id);
        store_.remove(store_.installedDir(id));
        return;
    }
    if (const auto deps = dependentsOf(id, have); !deps.empty()) {
        throw std::invalid_argument(id + " is required by " + joined(deps, ", ") + "; disable or remove those first");
    }
    store_.remove(it->second.dir);
}

}  // namespace rfmod
