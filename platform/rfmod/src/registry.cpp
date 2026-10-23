#include "rfmod/registry.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <system_error>

namespace rfmod {
namespace fs = std::filesystem;

namespace {

std::string joined(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : sep) + item;
    return out;
}

}  // namespace

ModuleRegistry::ModuleRegistry(RegistryConfig config, const TrustStore& trust, IHostServices& services)
    : config_(std::move(config)),
      store_(config_.root),
      trust_(trust),
      services_(services),
      verifier_(trust_, config_.hostVersion, config_.limits),
      loader_(LoaderConfig{config_.workerExecutable, config_.isolateAll, config_.policy}, store_.root(), services_) {}

ModuleRegistry::~ModuleRegistry() { shutdownAll(); }

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

void ModuleRegistry::shutdownAll() {
    std::vector<std::shared_ptr<IModuleInstance>> retired;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& id : std::ranges::reverse_view(order_)) {
            auto e = entries_.find(id);
            if (e != entries_.end() && e->second.instance) retired.push_back(std::move(e->second.instance));
        }
        entries_.clear();
        order_.clear();
        notices_.clear();
    }
    // In reverse dependency order, outside mutex_: shutdown is module code. Whoever still holds
    // an instance gets ModuleError "unavailable" from it from now on.
    for (const auto& instance : retired) shutdownLogged(*instance);
}

ModuleRegistry::Entries ModuleRegistry::readEntries(std::vector<std::string>& notices) const {
    Entries entries;
    for (auto& [id, installed] : InstalledReader(store_, verifier_).all(notices)) {
        Entry e{std::move(installed), ModuleState::Disabled, {}, nullptr, nullptr};
        if (const auto& schema = e.installed.manifest.optionsSchema) {
            try {
                e.optionsSchema = std::make_shared<const OptionsSchema>(json::parse(store_.readFile(e.installed.dir / *schema)));
            } catch (const std::invalid_argument& ex) {  // the schema cannot be used
                notices.push_back(id + ": options schema is unusable: " + ex.what());
            } catch (const json::exception& ex) {  // the schema is not JSON
                notices.push_back(id + ": options schema is not JSON: " + ex.what());
            }
        }
        // Options from state.json are checked again on every load: the file is the admin's,
        // but a schema can change with an upgrade.
        if (const auto why = e.optionsSchema ? e.optionsSchema->check(e.installed.settings.options)
                                             : std::optional<std::string>(std::nullopt);
            why || (!e.optionsSchema && !e.installed.settings.options.empty())) {
            notices.push_back(id + ": stored options no longer fit the module and were ignored" +
                              (why ? " (" + *why + ")" : ""));
            e.installed.settings.options = json::object();
        }
        entries.try_emplace(id, std::move(e));
    }
    return entries;
}

void ModuleRegistry::reloadLocked() {
    shutdownAll();
    std::vector<std::string> notices;
    Entries entries = readEntries(notices);
    DependencyResolver::Candidates candidates;
    for (const auto& [id, e] : entries) candidates.try_emplace(id, &e.installed.manifest, e.installed.settings.enabled);
    const std::vector<DependencyResolver::Step> plan = DependencyResolver(candidates, config_.hostVersion).plan();
    {
        const std::lock_guard lock(mutex_);
        entries_ = std::move(entries);
        notices_ = std::move(notices);
    }
    loadAll(plan);
    const std::lock_guard lock(mutex_);
    for (const auto& [id, e] : entries_) addNotices(id, e);
}

void ModuleRegistry::loadAll(const std::vector<DependencyResolver::Step>& plan) {
    for (const DependencyResolver::Step& step : plan) {
        Entry& e = entries_.at(step.id);
        if (step.state != ModuleState::Active) {
            const std::lock_guard lock(mutex_);
            e.state = step.state;
            e.stateDetail = step.detail;
            continue;
        }
        if (loadOne(e)) {
            const std::lock_guard lock(mutex_);
            order_.push_back(step.id);
        }
    }
}

bool ModuleRegistry::requiredDependenciesActive(const Entry& e, std::string& detail) const {
    for (const Dependency& d : e.installed.manifest.dependencies) {
        const auto dep = entries_.find(d.id);
        if (dep != entries_.end() && dep->second.state == ModuleState::Active) continue;
        detail = "needs " + d.id + " " + d.range.str() + " (" +
                 (dep == entries_.end() ? std::string("not installed") : toString(dep->second.state)) + ")";
        return false;
    }
    return true;
}

bool ModuleRegistry::loadOne(Entry& e) {
    // mutex_ is not held while the module loads: its init is module code and may read the registry.
    std::string detail;
    std::shared_ptr<IModuleInstance> instance;
    ModuleState state = ModuleState::MissingDependency;
    if (requiredDependenciesActive(e, detail)) {
        try {
            instance = loader_.load(e.installed.manifest, e.installed.signer, e.installed.dir, e.installed.settings.options, false);
            state = ModuleState::Active;
        } catch (const ModuleError& ex) {
            detail = ex.what();
            state = ModuleState::Failed;
        } catch (const json::exception& ex) {  // a malformed manifest or worker reply
            detail = ex.what();
            state = ModuleState::Failed;
        } catch (const PathEscapeError& ex) {  // a library path that leaves the module root
            detail = ex.what();
            state = ModuleState::Failed;
        } catch (const std::system_error& ex) {  // no worker process could be started
            detail = ex.what();
            state = ModuleState::Failed;
        }
    }
    const std::lock_guard lock(mutex_);
    e.instance = std::move(instance);
    e.state = state;
    e.stateDetail = detail;
    return state == ModuleState::Active;
}

void ModuleRegistry::addNotices(const std::string& id, const Entry& e) {
    using enum ModuleState;
    if (e.state == MissingDependency || e.state == Failed) {
        notices_.push_back(id + " is " + toString(e.state) + ": " + e.stateDetail);
        return;
    }
    if (e.state != Active) return;
    const Manifest& m = e.installed.manifest;
    if (m.deprecation.has_value()) {
        const auto& d = *m.deprecation;
        notices_.push_back(id + " is deprecated since " + d.since.str() + " and will be removed in " + d.removedIn.str() +
                           ". " + d.message + (d.replacement.empty() ? "" : " Use " + d.replacement + " instead."));
    }
    for (const Dependency& dep : m.dependencies) {
        if (const Entry& target = entries_.at(dep.id); target.installed.manifest.deprecation.has_value()) {
            notices_.push_back(id + " depends on deprecated " + dep.id + "; it stops working when " + dep.id +
                               " is removed in " + target.installed.manifest.deprecation->removedIn.str() + ".");
        }
    }
}

void ModuleRegistry::reload() {
    const std::lock_guard admin(adminMutex_);
    reloadLocked();
}

std::vector<std::string> ModuleRegistry::dependentsOf(std::string_view id) const {
    std::vector<std::string> out;
    for (const auto& [otherId, e] : entries_) {
        if (!e.installed.settings.enabled) continue;
        for (const Dependency& d : e.installed.manifest.dependencies) {
            if (d.id == id) out.push_back(otherId);
        }
    }
    return out;
}

void ModuleRegistry::updateSettings(const std::string& id, const ModuleSettings& settings) {
    const Entry& e = entries_.at(id);
    store_.writeSettings(e.installed.dir, settings, e.installed.manifest, e.installed.signer);
    reloadLocked();
}

void ModuleRegistry::setEnabled(const std::string& id, bool enabled) {
    const std::lock_guard admin(adminMutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end()) throw std::invalid_argument("unknown module " + id);
    if (!enabled) {
        if (const auto deps = dependentsOf(id); !deps.empty()) {
            throw std::invalid_argument(id + " is required by " + joined(deps, ", ") + "; disable that first");
        }
    }
    ModuleSettings settings = it->second.installed.settings;
    settings.enabled = enabled;
    updateSettings(id, settings);
}

void ModuleRegistry::setOptions(const std::string& id, const json& options) {
    const std::lock_guard admin(adminMutex_);
    if (const auto why = checkOptions(id, options)) throw std::invalid_argument(*why);
    ModuleSettings settings = entries_.at(id).installed.settings;
    settings.options = options;
    updateSettings(id, settings);
}

std::optional<std::string> ModuleRegistry::checkOptions(const std::string& id, const json& options) const {
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end()) return "unknown module " + id;
    if (!options.is_object()) return id + ": options must be an object";
    if (!it->second.optionsSchema) {
        if (it->second.installed.manifest.optionsSchema) return id + ": options schema is unusable";
        if (!options.empty()) return id + ": module takes no options";
        return std::nullopt;
    }
    if (const auto why = it->second.optionsSchema->check(options)) return id + ": " + *why;
    return std::nullopt;
}

json ModuleRegistry::describe(const Entry& e) const {
    const Manifest& m = e.installed.manifest;
    json info = {{"id", m.id},
                 {"version", m.version.str()},
                 {"title", m.title},
                 {"lifecycle", toString(m.lifecycle)},
                 {"provides", m.provides},
                 {"signedBy", e.installed.signer.label + " (" + e.installed.signer.key.id() + ")"},
                 {"isolated", e.instance ? e.instance->isolated() : loader_.isolates(e.installed.manifest, e.installed.signer)},
                 {"enabled", e.installed.settings.enabled},
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
    if (m.ui) info["ui"] = {{"bundle", m.ui->bundle}, {"slots", m.ui->slots}};
    if (e.optionsSchema) info["options"] = e.optionsSchema->document();
    return info;
}

json ModuleRegistry::list() const {
    const std::lock_guard lock(mutex_);
    json out = json::array();
    for (const auto& e : entries_ | std::views::values) out.push_back(describe(e));
    return out;
}

std::vector<std::string> ModuleRegistry::notices() const {
    const std::lock_guard lock(mutex_);
    return notices_;
}

std::vector<std::shared_ptr<IModuleInstance>> ModuleRegistry::withCapability(const std::string& capability) const {
    const std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<IModuleInstance>> out;
    for (const std::string& id : order_) {
        const Entry& e = entries_.at(id);
        if (e.instance && e.installed.manifest.hasCapability(capability)) out.push_back(e.instance);
    }
    return out;
}

std::shared_ptr<IModuleInstance> ModuleRegistry::instance(const std::string& id) const {
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.instance;
}

std::optional<fs::path> ModuleRegistry::publicFile(const std::string& id, const std::string& path) const {
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state != ModuleState::Active) return std::nullopt;
    try {
        // Canonical, so ".." and symlinks are resolved before the check: the file must be the
        // options schema itself or lie under ui/; never lib/ or anything outside the module.
        const fs::path dir = store_.root().confine(it->second.installed.dir);
        fs::path p = store_.root().confine(dir / path);
        const auto& schema = it->second.installed.manifest.optionsSchema;
        const bool isSchema = schema && p == store_.root().confine(dir / *schema);
        const bool isUi = p.string().starts_with((dir / "ui" / "").string());
        if ((!isUi && !isSchema) || !fs::is_regular_file(p)) return std::nullopt;
        return p;
    } catch (const PathEscapeError&) {
        return std::nullopt;
    }
}

}  // namespace rfmod
