#include "region_catalog.hpp"

#include "file_lock.hpp"
#include "security.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <system_error>

namespace rfmap {

namespace fs = std::filesystem;

std::string_view regionStateName(RegionState state) {
    switch (state) {
        case RegionState::Ready: return "ready";
        case RegionState::Building: return "building";
        case RegionState::Failed: return "failed";
    }
    return "unknown";
}

RegionCatalog::RegionCatalog(Settings settings, Logger& log, const IClock& clock)
    : settings_(std::move(settings)),
      log_(log),
      clock_(clock),
      store_(settings_.packageDir),
      world_(settings_.worldDir),
      pmtiles_(settings_.pmtiles) {
    if (!enabled()) return;
    log_.info("regions", std::format("offline regions of {} cut from {}, packages in {}", settings_.definitions.string(),
                                     settings_.worldDir.string(), settings_.packageDir.string()));
    if (!settings_.background) return;
    pending_ = true;  // the first scan right away
    worker_ = std::jthread([this] { run(); });
}

RegionCatalog::~RegionCatalog() {
    stop_.request_stop();  // a build stops at its next chunk or tool poll; wake_ wakes on it
}

void RegionCatalog::reload() {
    if (!enabled()) return;
    {
        const std::scoped_lock lock(wakeMutex_);
        pending_ = true;
    }
    wake_.notify_all();
}

void RegionCatalog::scan() {
    if (enabled()) scanWith(stop_.get_token());
}

void RegionCatalog::run() {
    const std::stop_token stop = stop_.get_token();
    const auto interval = std::chrono::minutes(settings_.rescanMinutes);
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(wakeMutex_);
            const auto ready = [this] { return pending_; };
            if (settings_.rescanMinutes > 0) wake_.wait_for(lock, stop, interval, ready);
            else wake_.wait(lock, stop, ready);
            if (stop.stop_requested()) return;
            pending_ = false;
        }
        try {
            scanWith(stop);
        } catch (const std::system_error& e) {  // the package directory or its lock file
            log_.error("regions", std::string("scan failed: ") + e.what());
        } catch (const json::exception& e) {  // an input of an unexpected shape that no build caught
            log_.error("regions", std::string("scan failed: ") + e.what());
        }
    }
}

std::optional<std::vector<RegionDefinition>> RegionCatalog::definitions() const {
    try {
        const RegionDefinitions file(settings_.definitions);
        for (const RegionDefinitions::Skip& s : file.skipped()) {
            log_.warn("regions", std::format("region '{}' skipped: {}", s.id, s.why));
        }
        return file.regions();
    } catch (const RegionError& e) {
        log_.error("regions", std::string(e.what()) + "; the packages already built stay as they are");
        return std::nullopt;
    }
}

void RegionCatalog::scanWith(const std::stop_token& stop) {
    const std::scoped_lock lock(scanMutex_);
    fs::create_directories(settings_.packageDir);
    const FileLock processes(store_.lockFile(), FileLock::Mode::Exclusive);
    const auto loaded = definitions();
    if (!loaded) {
        recover();
        return;
    }
    const std::vector<RegionDefinition>& found = *loaded;
    retain(found);
    std::string tool;
    try {
        tool = pmtiles_.version(settings_.packageDir / ".pmtiles-version.log");
    } catch (const RegionError& e) {
        failAll(found, e.what());
        return;
    }
    const auto pending = prime(found, tool);
    const RegionPackageBuilder builder({world_, pmtiles_, store_, clock_, log_}, stop);
    for (const Pending& p : pending) {
        if (!build(p, builder)) return;  // stopping: keep what is on disk for the next start
    }
    std::set<std::string, std::less<>> ids;
    for (const RegionDefinition& region : found) ids.insert(region.id());
    if (const std::size_t removed = store_.removeUnused(ids); removed > 0) {
        log_.info("regions", std::format("removed {} unused package files", removed));
    }
    const std::scoped_lock done(mutex_);
    ++scans_;
}

std::vector<RegionCatalog::Pending> RegionCatalog::prime(const std::vector<RegionDefinition>& found,
                                                         const std::string& tool) {
    std::vector<Pending> pending;
    for (const RegionDefinition& region : found) {
        const std::string text =
            std::format("format {}\ntool {}\n{}\n{}", RegionPackageBuilder::kFormat, tool, region.canonical(),
                        world_.fingerprint(RegionPackageBuilder::layersOf(region, world_)));
        std::string fingerprint = Secrets::hex(Secrets::sha256(text));
        auto package = store_.load(region.id());
        const bool fresh = package && package->fingerprint == fingerprint;
        set({region.id(), fresh ? RegionState::Ready : RegionState::Building, {}, std::move(package)});
        if (!fresh) pending.emplace_back(&region, std::move(fingerprint));
    }
    return pending;
}

bool RegionCatalog::build(const Pending& pending, const RegionPackageBuilder& builder) {
    const std::string& id = pending.region->id();
    RegionStatus status{id, RegionState::Ready, {}, find(id)};
    log_.info("regions", std::format("{}: building its package", id));
    try {
        status.package = builder.build(*pending.region, pending.fingerprint);
        log_.info("regions", std::format("{}: package {} ({} bytes)", id, status.package->version, status.package->size));
    } catch (const RegionBuildCancelled&) {
        return false;
    } catch (const RegionError& e) {
        failed(status, e.what());
    } catch (const std::system_error& e) {  // the work directory: disk full, permissions
        failed(status, e.what());
    } catch (const json::exception& e) {  // a world style of an unexpected shape
        failed(status, e.what());
    }
    set(std::move(status));
    return true;
}

void RegionCatalog::failed(RegionStatus& status, const std::string& why) const {
    status.state = RegionState::Failed;
    status.error = why;
    log_.error("regions", std::format("{}: package not built: {}", status.id, why));
}

void RegionCatalog::failAll(const std::vector<RegionDefinition>& found, const std::string& why) {
    log_.error("regions", "no packages built: " + why);
    for (const RegionDefinition& region : found) {
        set({region.id(), RegionState::Failed, why, store_.load(region.id())});
    }
}

void RegionCatalog::recover() {
    std::vector<RegionPackage> packages = store_.loadAll();
    const std::scoped_lock lock(mutex_);
    for (RegionPackage& p : packages) {
        if (regions_.contains(p.id)) continue;
        const std::string id = p.id;
        regions_.try_emplace(id, RegionStatus{id, RegionState::Ready, {}, std::move(p)});
    }
}

void RegionCatalog::set(RegionStatus status) {
    const std::scoped_lock lock(mutex_);
    const std::string id = status.id;
    regions_.insert_or_assign(id, std::move(status));
}

void RegionCatalog::retain(const std::vector<RegionDefinition>& found) {
    const std::scoped_lock lock(mutex_);
    std::erase_if(regions_, [&found](const auto& entry) {
        return std::ranges::none_of(found, [&entry](const RegionDefinition& r) { return r.id() == entry.first; });
    });
}

std::vector<RegionPackage> RegionCatalog::packages() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RegionPackage> out;
    for (const auto& [id, status] : regions_) {
        if (status.package) out.push_back(*status.package);
    }
    return out;
}

std::optional<RegionPackage> RegionCatalog::find(std::string_view id) const {
    const std::scoped_lock lock(mutex_);
    const auto it = regions_.find(id);
    if (it == regions_.end()) return std::nullopt;
    return it->second.package;
}

std::vector<RegionStatus> RegionCatalog::statuses() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RegionStatus> out;
    out.reserve(regions_.size());
    for (const auto& [id, status] : regions_) out.push_back(status);
    return out;
}

std::uint64_t RegionCatalog::scans() const {
    const std::scoped_lock lock(mutex_);
    return scans_;
}

}  // namespace rfmap
