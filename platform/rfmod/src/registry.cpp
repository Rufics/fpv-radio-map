#include "rfmod/registry.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
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
        Entry e{std::move(installed), ModuleState::Disabled, {}, nullptr, nullptr, {}};
        e.dir = openDir(id, e.installed.dir, notices);
        if (const auto& schema = e.installed.manifest.optionsSchema) {
            try {
                e.optionsSchema = std::make_shared<const OptionsSchema>(json::parse(store_.readFile(e.installed.dir / *schema)));
            } catch (const std::invalid_argument& ex) {  // the schema cannot be used
                notices.push_back(id + ": options schema is unusable: " + ex.what());
            } catch (const json::exception& ex) {  // the schema is not JSON
                notices.push_back(id + ": options schema is not JSON: " + ex.what());
            } catch (const PathEscapeError& ex) {  // the manifest points outside the module
                notices.push_back(id + ": options schema path is invalid: " + ex.what());
            } catch (const std::system_error& ex) {  // the file system refused the read
                notices.push_back(id + ": options schema cannot be read: " + ex.what());
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
    // Read and plan first: a read that throws (an unpack that fails, say) leaves the running
    // modules in place. Only once the new state is planned do the old instances go.
    std::vector<std::string> notices;
    Entries entries = readEntries(notices);
    DependencyResolver::Candidates candidates;
    for (const auto& [id, e] : entries) candidates.try_emplace(id, &e.installed.manifest, e.installed.settings.enabled);
    const std::vector<DependencyResolver::Step> plan = DependencyResolver(candidates, config_.hostVersion).plan();
    shutdownAll();
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
    using enum ModuleState;
    std::string detail;
    std::shared_ptr<IModuleInstance> instance;
    ModuleState state = MissingDependency;
    if (requiredDependenciesActive(e, detail)) {
        try {
            instance = loader_.load(e.installed.manifest, e.installed.signer, e.installed.dir, e.installed.settings.options, false);
            state = Active;
        } catch (const ModuleError& ex) {
            detail = ex.what();
            state = Failed;
        } catch (const json::exception& ex) {  // a malformed manifest or worker reply
            detail = ex.what();
            state = Failed;
        } catch (const PathEscapeError& ex) {  // a library path that leaves the module root
            detail = ex.what();
            state = Failed;
        } catch (const std::system_error& ex) {  // no worker process could be started
            detail = ex.what();
            state = Failed;
        }
    }
    const std::lock_guard lock(mutex_);
    e.instance = std::move(instance);
    e.state = state;
    e.stateDetail = detail;
    return state == Active;
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

FileDescriptor ModuleRegistry::openDir(const std::string& id, const fs::path& dir, std::vector<std::string>& notices) const {
    FileDescriptor fd(::open(store_.root().confine(dir).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) notices.push_back(id + ": its directory cannot be opened, so its files are not served");
    return fd;
}

std::optional<fs::path> ModuleRegistry::servablePath(const Entry& e, const std::string& path) {
    // Normalised lexically, never canonically: the directory on disk may already be another
    // version's, and the descriptor in the entry is the one the path is opened against.
    const fs::path rel = fs::path(path).lexically_normal();
    if (rel.empty() || rel.is_absolute() || !rel.has_filename()) return std::nullopt;
    if (std::ranges::any_of(rel, [](const fs::path& part) { return part == ".."; })) return std::nullopt;
    const auto& schema = e.installed.manifest.optionsSchema;
    const bool isSchema = schema && rel == fs::path(*schema).lexically_normal();
    const bool isUi = *rel.begin() == "ui" && rel != "ui";
    return (isUi || isSchema) ? std::optional(rel) : std::nullopt;
}

namespace {

/// A regular file opened under `dir` one path part at a time: no part may be a symlink, every
/// part but the last must be a directory, and the last must be a regular file; none otherwise.
/// O_NONBLOCK keeps a FIFO from stalling the open before fstat can reject it.
FileDescriptor openRegular(const FileDescriptor& dir, const fs::path& rel) {
    constexpr int kFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    FileDescriptor current;
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        const bool last = std::next(it) == rel.end();
        const int parent = current.valid() ? current.get() : dir.get();
        FileDescriptor next(::openat(parent, it->c_str(), last ? kFlags : (kFlags | O_DIRECTORY)));
        if (!next.valid()) return {};
        current = std::move(next);
    }
    if (struct stat st{}; !current.valid() || ::fstat(current.get(), &st) != 0 || !S_ISREG(st.st_mode)) return {};
    return current;
}

std::string readAll(const FileDescriptor& fd) {
    std::string out;
    std::array<char, 65536> buf{};
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) throw std::system_error(errno, std::generic_category(), "read module file");
        if (n == 0) return out;
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
}

}  // namespace

std::optional<ModuleRegistry::PublicFile> ModuleRegistry::publicFile(const std::string& id, const std::string& path) const {
    FileDescriptor file;
    fs::path rel;
    Lifecycle lifecycle{};
    {
        const std::lock_guard lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.state != ModuleState::Active || !it->second.dir.valid()) return std::nullopt;
        const auto servable = servablePath(it->second, path);
        if (!servable) return std::nullopt;
        // Opened relative to the directory the entry was loaded from: an install that renamed a
        // new version into place since then is not visible through this descriptor.
        file = openRegular(it->second.dir, *servable);
        if (!file.valid()) return std::nullopt;
        rel = *servable;
        lifecycle = it->second.installed.manifest.lifecycle;
    }
    return PublicFile{std::move(rel), lifecycle, readAll(file)};
}

}  // namespace rfmod
