#include "rfmod/registry.hpp"

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>

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

void writeFileAtomic(const fs::path& p, const std::string& content) {
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, p);
}

std::string randomName() {
    std::random_device rd;
    std::ostringstream s;
    s << std::hex << rd() << rd();
    return s.str();
}

std::string isoNow() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
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
    } catch (const std::exception& e) {
        return std::string("options rejected: ") + e.what();
    }
}

}  // namespace

std::string toString(ModuleState s) {
    switch (s) {
        case ModuleState::Active: return "active";
        case ModuleState::Disabled: return "disabled";
        case ModuleState::MissingDependency: return "missing-dependency";
        case ModuleState::Failed: return "failed";
    }
    return "failed";
}

ModuleRegistry::ModuleRegistry(RegistryConfig config, const TrustStore& trust, IHostServices& services)
    : config_(std::move(config)), trust_(trust), services_(services) {
    fs::create_directories(config_.root / "installed");
    fs::remove_all(config_.root / "staging");
    fs::create_directories(config_.root / "staging");
}

ModuleRegistry::~ModuleRegistry() {
    // Shut down in reverse dependency order.
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        auto e = entries_.find(*it);
        if (e != entries_.end() && e->second.instance) {
            try {
                e->second.instance->shutdown();
            } catch (...) {
            }
            e->second.instance.reset();
        }
    }
}

void ModuleRegistry::unpack(const PackageFiles& files, const fs::path& dir) {
    for (const auto& [path, content] : files.files) {
        // Write a new file and rename it into place, never rewrite in place: a library that is
        // still mapped by a running module must keep its old pages.
        const fs::path target = dir / path;
        fs::create_directories(target.parent_path());
        writeFileAtomic(target, content);
    }
}

std::unique_ptr<IModuleInstance> ModuleRegistry::load(const Manifest& m, const TrustedKey& signer, const fs::path& dir,
                                                      const json& options, bool forceIsolated) {
    const fs::path library = dir / m.entry.at(currentPlatform());
    if (signer.firstParty() && !forceIsolated && !config_.isolateAll) {
        return std::make_unique<InProcessModule>(m, library, services_, options);
    }
    return std::make_unique<IsolatedModule>(m, library, config_.workerExecutable, services_, options);
}

ValidationReport ModuleRegistry::upload(const std::string& archive, bool install) {
    std::lock_guard admin(adminMutex_);
    ValidationReport report;
    auto pkg = PackageVerifier::verify(archive, trust_, config_.hostVersion, report, config_.limits);
    if (!pkg) return report;
    const Manifest& m = pkg->manifest;

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
        } else if (it->second.manifest.lifecycle == Lifecycle::Deprecated) {
            report.pass("dependencies.deprecated", d.id + " is deprecated: " + it->second.manifest.deprecation->message);
        }
    }
    std::string optionalDetail;
    for (const Dependency& d : m.optionalDependencies) {
        const auto it = entries_.find(d.id);
        const bool ok = it != entries_.end() && d.range.contains(it->second.manifest.version);
        optionalDetail += (optionalDetail.empty() ? "" : "; ") + d.id + (ok ? " present" : " absent, features off");
    }
    if (problems.empty()) {
        report.pass("dependencies", m.dependencies.empty() ? "no required dependencies"
                                                            : std::to_string(m.dependencies.size()) + " required, all active" +
                                                                  (optionalDetail.empty() ? "" : "; optional: " + optionalDetail));
    } else {
        std::string all;
        for (const auto& p : problems) all += (all.empty() ? "" : "; ") + p;
        report.fail("dependencies", all);
    }
    if (m.lifecycle == Lifecycle::Deprecated) {
        report.pass("lifecycle", "deprecated since " + m.deprecation->since.str() + ", removed in " +
                                     m.deprecation->removedIn.str() + ": " + m.deprecation->message);
        if (config_.hostVersion >= m.deprecation->removedIn) {
            report.fail("lifecycle.removed", "this host (" + config_.hostVersion.str() + ") no longer accepts it");
        }
    } else {
        report.pass("lifecycle", toString(m.lifecycle));
    }
    if (!report.ok()) return report;

    // Self-test always runs in a worker, whoever signed it: an upload is untrusted until installed.
    const fs::path staging = config_.root / "staging" / randomName();
    try {
        unpack(pkg->files, staging);
        auto instance = load(m, pkg->signer, staging, json::object(), true);
        if (const auto* iso = dynamic_cast<const IsolatedModule*>(instance.get())) {
            const SandboxInfo& sb = iso->sandbox();
            // Informational: says what this server's kernel allowed, so nobody assumes more.
            report.pass("sandbox", "separate process, memory limit " + std::to_string(sb.memoryLimitMb) +
                                       " MB, no file writes; no_new_privs " + (sb.noNewPrivileges ? "on" : "unavailable") +
                                       "; network " + (sb.networkIsolated ? "cut off" : "NOT isolated on this host"));
        }
        const json result = instance->invoke("rfmod.selftest", json::object());
        instance->shutdown();
        const bool passed = result.value("ok", false);
        std::string detail = passed ? "self-test passed" : "self-test failed";
        if (result.contains("checks")) {
            for (const json& c : result.at("checks")) {
                detail += "; " + c.value("name", "?") + (c.value("ok", false) ? " ok" : " FAILED");
            }
        }
        report.add("selftest", passed, detail);
    } catch (const ModuleError& e) {
        report.fail("selftest", e.code() + ": " + e.what());
    } catch (const std::exception& e) {
        report.fail("selftest", e.what());
    }

    if (!report.ok() || !install) {
        fs::remove_all(staging);
        return report;
    }

    // Install: replace the old version atomically from the directory's point of view.
    const fs::path target = config_.root / "installed" / m.id;
    writeFileAtomic(staging / kPackageFile, archive);
    Entry fresh{m, pkg->signer, target, true, json::object(), ModuleState::Disabled, {}, nullptr};
    if (auto existing = entries_.find(m.id); existing != entries_.end()) {
        retire(existing->second);
        fresh.enabled = existing->second.enabled;
        if (!validateOptions(staging, m, existing->second.options)) fresh.options = existing->second.options;
    }
    const fs::path old = config_.root / "staging" / (m.id + ".old." + randomName());
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(staging, target);
    fs::remove_all(old);
    writeState(fresh);
    report.pass("installed", m.id + " " + m.version.str() + " at " + isoNow());

    reloadLocked();
    return report;
}

void ModuleRegistry::writeState(const Entry& e) const {
    const json state = {{"enabled", e.enabled}, {"options", e.options}, {"signer", e.signer.key.id()},
                        {"version", e.manifest.version.str()}, {"updatedAt", isoNow()}};
    writeFileAtomic(e.dir / kStateFile, state.dump(2) + "\n");
}

std::vector<std::string> ModuleRegistry::dependentsOf(const std::string& id) const {
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
    fs::remove_all(it->second.dir);
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
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            auto e = entries_.find(*it);
            if (e != entries_.end() && e->second.instance) retired.push_back(std::move(e->second.instance));
        }
        entries_.clear();
        order_.clear();
        notices_.clear();
    }
    // In reverse dependency order, outside mutex_: shutdown is module code.
    for (const auto& instance : retired) {
        try {
            instance->shutdown();
        } catch (...) {
        }
    }
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
    for (const auto& item : fs::directory_iterator(config_.root / "installed")) {
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

void ModuleRegistry::resolve() {
    // Depth-first over required and optional dependencies; required failures propagate.
    // mutex_ is released only while a module loads: its init is module code and may read the registry.
    std::unique_lock lock(mutex_);
    std::map<std::string, int, std::less<>> mark;  // 0 = new, 1 = visiting, 2 = done
    std::function<bool(const std::string&)> visit = [this, &visit, &mark, &lock](const std::string& id) -> bool {
        Entry& e = entries_.at(id);
        if (mark[id] == 2) return e.state == ModuleState::Active;
        if (mark[id] == 1) {
            e.state = ModuleState::Failed;
            e.stateDetail = "dependency cycle through " + id;
            return false;
        }
        mark[id] = 1;
        bool ok = e.enabled;
        if (!e.enabled) {
            e.state = ModuleState::Disabled;
            e.stateDetail = "disabled by the admin";
        }
        if (ok && e.manifest.deprecation && config_.hostVersion >= e.manifest.deprecation->removedIn) {
            ok = false;
            e.state = ModuleState::Failed;
            e.stateDetail = "removed in host " + e.manifest.deprecation->removedIn.str();
        }
        for (const Dependency& d : e.manifest.dependencies) {
            if (!ok) break;
            const auto dep = entries_.find(d.id);
            if (dep == entries_.end() || !d.range.contains(dep->second.manifest.version) || !visit(d.id)) {
                ok = false;
                e.state = ModuleState::MissingDependency;
                e.stateDetail = "needs " + d.id + " " + d.range.str() +
                                (dep == entries_.end() ? " (not installed)"
                                                       : " (installed " + dep->second.manifest.version.str() + ", " +
                                                             toString(dep->second.state) + ")");
            }
        }
        for (const Dependency& d : e.manifest.optionalDependencies) {
            if (!ok) break;
            if (entries_.count(d.id) && d.range.contains(entries_.at(d.id).manifest.version)) (void)visit(d.id);
        }
        if (ok) {
            std::unique_ptr<IModuleInstance> instance;
            std::string error;
            lock.unlock();
            try {
                instance = load(e.manifest, e.signer, e.dir, e.options, false);
            } catch (const std::exception& ex) {
                error = ex.what();
            }
            lock.lock();
            if (instance) {
                e.instance = std::move(instance);
                e.state = ModuleState::Active;
                e.stateDetail.clear();
                order_.push_back(id);
            } else {
                e.state = ModuleState::Failed;
                e.stateDetail = error;
                ok = false;
            }
        }
        mark[id] = 2;
        return ok;
    };
    for (auto& [id, e] : entries_) (void)visit(id);

    for (const auto& [id, e] : entries_) {
        if (e.state == ModuleState::Active && e.manifest.deprecation) {
            const auto& d = *e.manifest.deprecation;
            notices_.push_back(id + " is deprecated since " + d.since.str() + " and will be removed in " +
                               d.removedIn.str() + ". " + d.message +
                               (d.replacement.empty() ? "" : " Use " + d.replacement + " instead."));
        }
        if (e.state == ModuleState::Active) {
            for (const Dependency& dep : e.manifest.dependencies) {
                const Entry& target = entries_.at(dep.id);
                if (target.manifest.deprecation) {
                    notices_.push_back(id + " depends on deprecated " + dep.id + "; it stops working when " + dep.id +
                                       " is removed in " + target.manifest.deprecation->removedIn.str() + ".");
                }
            }
        }
        if (e.state == ModuleState::MissingDependency || e.state == ModuleState::Failed) {
            notices_.push_back(id + " is " + toString(e.state) + ": " + e.stateDetail);
        }
    }
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
    const bool isUi = path.rfind("ui/", 0) == 0;
    const bool isSchema = it->second.manifest.optionsSchema && path == *it->second.manifest.optionsSchema;
    if ((!isUi && !isSchema) || path.find("..") != std::string::npos) return std::nullopt;
    const fs::path p = it->second.dir / path;
    if (!fs::is_regular_file(p)) return std::nullopt;
    return p;
}

}  // namespace rfmod
