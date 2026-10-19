#include "rfmod/registry.hpp"

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

namespace rfmod {
namespace fs = std::filesystem;

namespace {

constexpr const char* kPackageFile = "package.rfmod";
constexpr const char* kStateFile = "state.json";

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// Writes `p` (which must lie inside `root`) through a temporary file renamed into place.
void writeFileAtomic(const ConfinedRoot& root, const fs::path& p, std::string_view content) {
    const fs::path target = root.confine(p);
    const fs::path tmp = root.confine(target.string() + ".tmp");
    fs::create_directories(target.parent_path());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw RegistryWriteError("cannot write " + tmp.string());
    }
    fs::rename(tmp, target);
}

std::string randomName() {
    std::random_device rd;
    std::ostringstream s;
    s << std::hex << rd() << rd();
    return s.str();
}

std::string isoNow() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::optional<std::string> validateOptions(const fs::path& dir, const Manifest& m, const json& options) {
    if (!m.optionsSchema) {
        if (options.is_object() && options.empty()) return std::nullopt;
        return "module takes no options";
    }
    try {
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(json::parse(readFile(dir / *m.optionsSchema)));
        v.validate(options);
        return std::nullopt;
    } catch (const std::invalid_argument& e) {  // the validator's verdict, or a schema it cannot use
        return std::string("options rejected: ") + e.what();
    } catch (const json::exception& e) {  // the options schema is not JSON
        return std::string("options rejected: ") + e.what();
    }
}

void checkLifecycle(const Manifest& m, const Version& hostVersion, ValidationReport& report) {
    if (m.lifecycle != Lifecycle::Deprecated || !m.deprecation.has_value()) {
        report.pass("lifecycle", toString(m.lifecycle));
        return;
    }
    const Deprecation& d = *m.deprecation;
    report.pass("lifecycle", "deprecated since " + d.since.str() + ", removed in " + d.removedIn.str() + ": " + d.message);
    if (hostVersion >= d.removedIn) {
        report.fail("lifecycle.removed", "this host (" + hostVersion.str() + ") no longer accepts it");
    }
}

std::string sandboxDetail(const SandboxInfo& sb) {
    return std::format("separate process, memory limit {} MB, no file writes; no_new_privs {}; network {}", sb.memoryLimitMb,
                       sb.noNewPrivileges ? "on" : "unavailable", sb.networkIsolated ? "cut off" : "NOT isolated on this host");
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

}  // namespace

std::string toString(ModuleState s) {
    using enum ModuleState;
    switch (s) {
        case Active: return "active";
        case Disabled: return "disabled";
        case MissingDependency: return "missing-dependency";
        case Failed: return "failed";
    }
    return "failed";
}

ModuleRegistry::ModuleRegistry(RegistryConfig config, const TrustStore& trust, IHostServices& services)
    : config_(std::move(config)), root_(config_.root), trust_(trust), services_(services) {
    fs::create_directories(root_.resolve("installed"));
    const fs::path staging = root_.resolve("staging");
    fs::remove_all(staging);
    fs::create_directories(staging);
}

ModuleRegistry::~ModuleRegistry() {
    // Shut down in reverse dependency order.
    for (const auto& id : std::ranges::reverse_view(order_)) {
        auto e = entries_.find(id);
        if (e != entries_.end() && e->second.instance) {
            shutdownLogged(*e->second.instance);
            e->second.instance.reset();
        }
    }
}

void ModuleRegistry::shutdownLogged(IModuleInstance& instance) const {
    // One module failing to shut down must not stop the others, so the failure is only logged.
    try {
        instance.shutdown();
    } catch (const ModuleError& e) {
        services_.log(instance.manifest().id, RFMOD_LOG_WARN,
                      std::format("shutdown failed: {}: {}", e.code(), e.what()));
    } catch (const std::system_error& e) {
        services_.log(instance.manifest().id, RFMOD_LOG_WARN, std::format("shutdown failed: {}", e.what()));
    }
}

void ModuleRegistry::unpack(const PackageFiles& files, const fs::path& dir) const {
    for (const auto& [path, content] : files.files) {
        // Write a new file and rename it into place, never rewrite in place: a library that is
        // still mapped by a running module must keep its old pages.
        writeFileAtomic(root_, dir / path, content);
    }
}

std::unique_ptr<IModuleInstance> ModuleRegistry::load(const Manifest& m, const TrustedKey& signer, const fs::path& dir,
                                                      const json& options, bool forceIsolated) {
    const fs::path library = root_.confine(dir / m.entry.at(currentPlatform()));
    if (signer.firstParty() && !forceIsolated && !config_.isolateAll) {
        return std::make_unique<InProcessModule>(m, root_, library, services_, options);
    }
    return std::make_unique<IsolatedModule>(m, library, config_.workerExecutable, services_, options);
}

void ModuleRegistry::checkUpgrade(const Manifest& m, ValidationReport& report) const {
    if (auto existing = entries_.find(m.id); existing != entries_.end()) {
        const Version& have = existing->second.manifest.version;
        report.add("version.upgrade", m.version > have,
                   m.version > have ? "upgrades " + have.str() + " to " + m.version.str()
                                    : "version " + m.version.str() + " is not newer than installed " + have.str());
    }

    // An upgrade must not leave an enabled dependent outside the range it asked for.
    std::string stranded;
    for (const auto& [otherId, other] : entries_) {
        if (!other.enabled || otherId == m.id) continue;
        for (const Dependency& d : other.manifest.dependencies) {
            if (d.id == m.id && !d.range.contains(m.version)) {
                stranded += (stranded.empty() ? "" : "; ") + otherId + " needs " + d.range.str();
            }
        }
    }
    if (!stranded.empty()) report.fail("dependents", m.version.str() + " breaks enabled modules: " + stranded);
}

std::string ModuleRegistry::optionalDependencySummary(const Manifest& m) const {
    std::string optionalDetail;
    for (const Dependency& d : m.optionalDependencies) {
        const auto it = entries_.find(d.id);
        const bool ok = it != entries_.end() && d.range.contains(it->second.manifest.version);
        optionalDetail += (optionalDetail.empty() ? "" : "; ") + d.id + (ok ? " present" : " absent, features off");
    }
    return optionalDetail;
}

void ModuleRegistry::checkDependencies(const Manifest& m, ValidationReport& report) const {
    // Required dependencies must already be installed, in range and loadable.
    std::vector<std::string> problems;
    for (const Dependency& d : m.dependencies) {
        const auto it = entries_.find(d.id);
        if (it == entries_.end()) {
            problems.push_back(d.id + " " + d.range.str() + " is not installed");
        } else if (!d.range.contains(it->second.manifest.version)) {
            problems.push_back(d.id + " " + d.range.str() + " needed, " + it->second.manifest.version.str() + " installed");
        } else if (it->second.state != ModuleState::Active) {
            problems.push_back(d.id + " is " + toString(it->second.state));
        } else if (const auto& dep = it->second.manifest.deprecation;
                   it->second.manifest.lifecycle == Lifecycle::Deprecated && dep.has_value()) {
            report.pass("dependencies.deprecated", d.id + " is deprecated: " + dep->message);
        }
    }
    const std::string optionalDetail = optionalDependencySummary(m);
    if (problems.empty()) {
        std::string detail = "no required dependencies";
        if (!m.dependencies.empty()) {
            detail = std::format("{} required, all active", m.dependencies.size());
            if (!optionalDetail.empty()) detail += std::format("; optional: {}", optionalDetail);
        }
        report.pass("dependencies", detail);
    } else {
        std::string all;
        for (const auto& p : problems) all += (all.empty() ? "" : "; ") + p;
        report.fail("dependencies", all);
    }
}

void ModuleRegistry::runSelfTest(const VerifiedPackage& pkg, const fs::path& staging, ValidationReport& report) {
    try {
        unpack(pkg.files, staging);
        auto instance = load(pkg.manifest, pkg.signer, staging, json::object(), true);
        if (const auto* iso = dynamic_cast<const IsolatedModule*>(instance.get())) {
            // Informational: says what this server's kernel allowed, so nobody assumes more.
            report.pass("sandbox", sandboxDetail(iso->sandbox()));
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

void ModuleRegistry::installStaged(const VerifiedPackage& pkg, const std::string& archive, const fs::path& staging,
                                   ValidationReport& report) {
    // Replace the old version atomically from the directory's point of view.
    const Manifest& m = pkg.manifest;
    const fs::path target = root_.resolve(fs::path("installed") / m.id);
    const fs::path staged = root_.confine(staging);
    writeFileAtomic(root_, staged / kPackageFile, archive);
    Entry fresh{m, pkg.signer, target, true, json::object(), ModuleState::Disabled, {}, nullptr};
    if (auto existing = entries_.find(m.id); existing != entries_.end()) {
        retire(existing->second);
        fresh.enabled = existing->second.enabled;
        if (!validateOptions(staged, m, existing->second.options)) fresh.options = existing->second.options;
    }
    const fs::path old = root_.resolve(fs::path("staging") / (m.id + ".old." + randomName()));
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(staged, target);
    fs::remove_all(old);
    writeState(fresh);
    report.pass("installed", m.id + " " + m.version.str() + " at " + isoNow());
}

ValidationReport ModuleRegistry::upload(const std::string& archive, bool install) {
    const std::lock_guard admin(adminMutex_);
    ValidationReport report;
    auto pkg = PackageVerifier::verify(archive, trust_, config_.hostVersion, report, config_.limits);
    if (!pkg) return report;

    checkUpgrade(pkg->manifest, report);
    checkDependencies(pkg->manifest, report);
    checkLifecycle(pkg->manifest, config_.hostVersion, report);
    if (!report.ok()) return report;

    // Self-test always runs in a worker, whoever signed it: an upload is untrusted until installed.
    const fs::path staging = root_.resolve(fs::path("staging") / randomName());
    runSelfTest(*pkg, staging, report);
    if (!report.ok() || !install) {
        fs::remove_all(root_.confine(staging));
        return report;
    }

    installStaged(*pkg, archive, staging, report);
    reloadLocked();
    return report;
}

void ModuleRegistry::writeState(const Entry& e) const {
    const json state = {{"enabled", e.enabled}, {"options", e.options}, {"signer", e.signer.key.id()},
                        {"version", e.manifest.version.str()}, {"updatedAt", isoNow()}};
    writeFileAtomic(root_, e.dir / kStateFile, state.dump(2) + "\n");
}

std::vector<std::string> ModuleRegistry::dependentsOf(std::string_view id) const {
    std::vector<std::string> out;
    for (const auto& [otherId, e] : entries_) {
        if (!e.enabled) continue;
        for (const Dependency& d : e.manifest.dependencies) {
            if (d.id == id) out.push_back(otherId);
        }
    }
    return out;
}

void ModuleRegistry::remove(const std::string& id) {
    std::lock_guard admin(adminMutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end()) throw std::invalid_argument("unknown module " + id);
    if (const auto deps = dependentsOf(id); !deps.empty()) {
        std::string list;
        for (const auto& d : deps) list += (list.empty() ? "" : ", ") + d;
        throw std::invalid_argument(id + " is required by " + list + "; disable or remove those first");
    }
    retire(it->second);
    fs::remove_all(root_.confine(it->second.dir));
    {
        std::lock_guard lock(mutex_);
        entries_.erase(it);
    }
    reloadLocked();
}

void ModuleRegistry::setEnabled(const std::string& id, bool enabled) {
    std::lock_guard admin(adminMutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end()) throw std::invalid_argument("unknown module " + id);
    if (!enabled) {
        if (const auto deps = dependentsOf(id); !deps.empty()) {
            throw std::invalid_argument(id + " is required by " + deps.front() + "; disable that first");
        }
    }
    {
        std::lock_guard lock(mutex_);
        it->second.enabled = enabled;
    }
    writeState(it->second);
    reloadLocked();
}

void ModuleRegistry::setOptions(const std::string& id, const json& options) {
    std::lock_guard admin(adminMutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end()) throw std::invalid_argument("unknown module " + id);
    if (auto err = validateOptions(it->second.dir, it->second.manifest, options)) throw std::invalid_argument(*err);
    {
        std::lock_guard lock(mutex_);
        it->second.options = options;
    }
    writeState(it->second);
    reloadLocked();
}

void ModuleRegistry::reload() {
    std::lock_guard admin(adminMutex_);
    reloadLocked();
}

void ModuleRegistry::retire(Entry& e) {
    std::unique_ptr<IModuleInstance> instance;
    {
        std::lock_guard lock(mutex_);
        instance = std::move(e.instance);
    }
    if (instance) instance->shutdown();
}

void ModuleRegistry::shutdownAll() {
    std::vector<std::unique_ptr<IModuleInstance>> retired;
    {
        std::lock_guard lock(mutex_);
        for (const auto& id : std::ranges::reverse_view(order_)) {
            auto e = entries_.find(id);
            if (e != entries_.end() && e->second.instance) retired.push_back(std::move(e->second.instance));
        }
        entries_.clear();
        order_.clear();
        notices_.clear();
    }
    // In reverse dependency order, outside mutex_: shutdown is module code.
    for (const auto& instance : retired) shutdownLogged(*instance);
}

std::optional<ModuleRegistry::Entry> ModuleRegistry::readInstalled(const fs::path& dir,
                                                                   std::vector<std::string>& notices) const {
    ValidationReport report;
    // Re-verify on every start: a removed key or a changed byte disables the module.
    auto pkg = PackageVerifier::verify(readFile(dir / kPackageFile), trust_, config_.hostVersion, report,
                                       config_.limits);
    if (!pkg) {
        std::string why;
        for (const auto& c : report.checks()) {
            if (!c.ok) why = c.name + ": " + c.detail;
        }
        notices.push_back("Module in " + dir.filename().string() + " was not loaded. " + why);
        return std::nullopt;
    }
    Entry e{pkg->manifest, pkg->signer, dir, true, json::object(), ModuleState::Disabled, {}, nullptr};
    if (fs::exists(dir / kStateFile)) {
        const json state = json::parse(readFile(dir / kStateFile));
        e.enabled = state.value("enabled", true);
        e.options = state.value("options", json::object());
    }
    // The files on disk must be the signed ones.
    if (const bool intact = std::ranges::all_of(pkg->files.files,
                                                [&dir](const auto& file) {
                                                    return fs::exists(dir / file.first) &&
                                                           readFile(dir / file.first) == file.second;
                                                });
        !intact) {
        unpack(pkg->files, dir);
        notices.push_back(e.manifest.id + ": unpacked files differed from the signed package and were restored");
    }
    return e;
}

void ModuleRegistry::reloadLocked() {
    shutdownAll();
    std::map<std::string, Entry, std::less<>> entries;
    std::vector<std::string> notices;
    for (const auto& item : fs::directory_iterator(root_.resolve("installed"))) {
        if (!item.is_directory()) continue;
        if (auto e = readInstalled(item.path(), notices)) {
            const std::string id = e->manifest.id;
            entries.try_emplace(id, std::move(*e));
        }
    }
    {
        std::lock_guard lock(mutex_);
        entries_ = std::move(entries);
        notices_ = std::move(notices);
    }
    resolve();
}

bool ModuleRegistry::visit(const std::string& id, VisitMarks& mark, std::unique_lock<std::mutex>& lock) {
    using enum ModuleState;
    Entry& e = entries_.at(id);
    if (mark[id] == 2) return e.state == Active;
    if (mark[id] == 1) {
        e.state = Failed;
        e.stateDetail = "dependency cycle through " + id;
        return false;
    }
    mark[id] = 1;
    bool ok = mayLoad(e) && requiredDependenciesActive(e, mark, lock);
    if (ok) {
        for (const Dependency& d : e.manifest.optionalDependencies) {
            if (entries_.contains(d.id) && d.range.contains(entries_.at(d.id).manifest.version)) (void)visit(d.id, mark, lock);
        }
        ok = start(id, e, lock);
    }
    mark[id] = 2;
    return ok;
}

bool ModuleRegistry::mayLoad(Entry& e) const {
    using enum ModuleState;
    if (!e.enabled) {
        e.state = Disabled;
        e.stateDetail = "disabled by the admin";
        return false;
    }
    if (e.manifest.deprecation && config_.hostVersion >= e.manifest.deprecation->removedIn) {
        e.state = Failed;
        e.stateDetail = "removed in host " + e.manifest.deprecation->removedIn.str();
        return false;
    }
    return true;
}

bool ModuleRegistry::requiredDependenciesActive(Entry& e, VisitMarks& mark, std::unique_lock<std::mutex>& lock) {
    for (const Dependency& d : e.manifest.dependencies) {
        const auto dep = entries_.find(d.id);
        if (dep != entries_.end() && d.range.contains(dep->second.manifest.version) && visit(d.id, mark, lock)) continue;
        e.state = ModuleState::MissingDependency;
        e.stateDetail = "needs " + d.id + " " + d.range.str() +
                        (dep == entries_.end() ? " (not installed)"
                                               : " (installed " + dep->second.manifest.version.str() + ", " +
                                                     toString(dep->second.state) + ")");
        return false;
    }
    return true;
}

bool ModuleRegistry::start(const std::string& id, Entry& e, std::unique_lock<std::mutex>& lock) {
    using enum ModuleState;
    std::unique_ptr<IModuleInstance> instance;
    std::string error;
    lock.unlock();
    try {
        instance = load(e.manifest, e.signer, e.dir, e.options, false);
    } catch (const ModuleError& ex) {
        error = ex.what();
    } catch (const json::exception& ex) {  // a malformed manifest or worker reply
        error = ex.what();
    } catch (const PathEscapeError& ex) {  // a library path that leaves the registry root
        error = ex.what();
    } catch (const std::system_error& ex) {  // no worker process could be started
        error = ex.what();
    }
    lock.lock();
    if (!instance) {
        e.state = Failed;
        e.stateDetail = error;
        return false;
    }
    e.instance = std::move(instance);
    e.state = Active;
    e.stateDetail.clear();
    order_.push_back(id);
    return true;
}

void ModuleRegistry::addNotices(const std::string& id, const Entry& e) {
    using enum ModuleState;
    if (e.state == MissingDependency || e.state == Failed) {
        notices_.push_back(id + " is " + toString(e.state) + ": " + e.stateDetail);
        return;
    }
    if (e.state != Active) return;
    if (e.manifest.deprecation.has_value()) {
        const auto& d = *e.manifest.deprecation;
        notices_.push_back(id + " is deprecated since " + d.since.str() + " and will be removed in " +
                           d.removedIn.str() + ". " + d.message +
                           (d.replacement.empty() ? "" : " Use " + d.replacement + " instead."));
    }
    for (const Dependency& dep : e.manifest.dependencies) {
        if (const Entry& target = entries_.at(dep.id); target.manifest.deprecation.has_value()) {
            notices_.push_back(id + " depends on deprecated " + dep.id + "; it stops working when " + dep.id +
                               " is removed in " + target.manifest.deprecation->removedIn.str() + ".");
        }
    }
}

void ModuleRegistry::resolve() {
    // Depth-first over required and optional dependencies; required failures propagate.
    // mutex_ is released only while a module loads: its init is module code and may read the registry.
    std::unique_lock lock(mutex_);
    VisitMarks mark;
    for (const auto& [id, e] : entries_) (void)visit(id, mark, lock);
    for (const auto& [id, e] : entries_) addNotices(id, e);
}

json ModuleRegistry::list() const {
    std::lock_guard lock(mutex_);
    json out = json::array();
    for (const auto& [id, e] : entries_) {
        const Manifest& m = e.manifest;
        json info = {{"id", m.id},
                     {"version", m.version.str()},
                     {"title", m.title},
                     {"lifecycle", toString(m.lifecycle)},
                     {"provides", m.provides},
                     {"signedBy", e.signer.label + " (" + e.signer.key.id() + ")"},
                     {"isolated", e.instance ? e.instance->isolated() : !(e.signer.firstParty() && !config_.isolateAll)},
                     {"state", toString(e.state)}};
        if (!e.stateDetail.empty()) info["stateDetail"] = e.stateDetail;
        json req = json::array();
        for (const Dependency& d : m.dependencies) req.push_back(d.id + " " + d.range.str());
        json opt = json::array();
        for (const Dependency& d : m.optionalDependencies) opt.push_back(d.id + " " + d.range.str());
        info["requires"] = req;
        info["optional"] = opt;
        if (m.deprecation) {
            info["deprecation"] = {{"since", m.deprecation->since.str()},
                                   {"removedIn", m.deprecation->removedIn.str()},
                                   {"replacement", m.deprecation->replacement},
                                   {"message", m.deprecation->message}};
        }
        if (m.ui) info["ui"] = {{"bundle", "/api/v1/modules/" + m.id + "/" + m.ui->bundle}, {"slots", m.ui->slots}};
        if (m.optionsSchema) info["options"] = json::parse(readFile(e.dir / *m.optionsSchema));
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<std::string> ModuleRegistry::notices() const {
    std::lock_guard lock(mutex_);
    return notices_;
}

std::vector<IModuleInstance*> ModuleRegistry::withCapability(const std::string& capability) const {
    std::lock_guard lock(mutex_);
    std::vector<IModuleInstance*> out;
    for (const std::string& id : order_) {
        const Entry& e = entries_.at(id);
        if (e.instance && e.manifest.hasCapability(capability)) out.push_back(e.instance.get());
    }
    return out;
}

IModuleInstance* ModuleRegistry::instance(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.instance.get();
}

std::optional<fs::path> ModuleRegistry::publicFile(const std::string& id, const std::string& path) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state != ModuleState::Active) return std::nullopt;
    // Only the UI bundle directory and the options schema are public; never lib/.
    const bool isUi = path.starts_with("ui/");
    if (const bool isSchema = it->second.manifest.optionsSchema == path;
        (!isUi && !isSchema) || path.find("..") != std::string::npos) {
        return std::nullopt;
    }
    fs::path p = it->second.dir / path;
    if (!fs::is_regular_file(p)) return std::nullopt;
    return p;
}

}  // namespace rfmod
