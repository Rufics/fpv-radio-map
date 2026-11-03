#include "region_areas.hpp"

#include "file_lock.hpp"
#include "security.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr auto kKeys = std::to_array<std::string_view>({"geometry", "name", "layers", "maxzoom"});
/// Hex digits of the request hash in an area id: 96 bits, far from any collision.
constexpr std::size_t kIdHexDigits = 24;
/// How long a failed build is remembered, so polling clients see why; asking again retries.
constexpr auto kFailureMemory = std::chrono::hours(1);
/// How often an idle worker removes unused packages.
constexpr auto kSweepInterval = std::chrono::hours(1);

/// `entry` read as a region; AreaRefused names what is wrong with it.
RegionDefinition parse(const json& entry) {
    try {
        return {entry, fs::path()};
    } catch (const RegionError& e) {
        throw AreaRefused(e.what());
    } catch (const json::exception& e) {
        throw AreaRefused(e.what());
    }
}

}  // namespace

std::string_view areaStateName(AreaState state) {
    using enum AreaState;
    switch (state) {
        case Queued: return "queued";
        case Building: return "building";
        case Ready: return "ready";
        case Failed: return "failed";
    }
    return "unknown";
}

RegionAreas::RegionAreas(Settings settings, Logger& log, const IClock& clock)
    : settings_(std::move(settings)),
      log_(log),
      clock_(clock),
      store_(settings_.areaDir),
      world_(settings_.worldDir),
      pmtiles_(settings_.pmtiles) {
    if (!enabled()) return;
    log_.info("regions", std::format("drawn areas up to {} km² built into {}", settings_.limits.maxAreaKm2,
                                     settings_.areaDir.string()));
    if (settings_.background) worker_ = std::jthread([this] { run(); });
}

RegionAreas::~RegionAreas() {
    stop_.request_stop();  // a build stops at its next chunk or tool poll; wake_ wakes on it
}

RegionDefinition RegionAreas::define(const json& body) const {
    if (!body.is_object() || !body.contains("geometry")) {
        throw AreaRefused("send {\"geometry\": a GeoJSON Polygon or MultiPolygon}");
    }
    for (const auto& [key, value] : body.items()) {
        if (std::ranges::find(kKeys, key) == kKeys.end()) throw AreaRefused(std::format("unknown field '{}'", key));
    }
    json entry = body;
    entry["id"] = "area";
    if (!entry.contains("name")) entry["name"] = kDefaultName;
    if (!entry.contains("maxzoom")) entry["maxzoom"] = settings_.limits.maxZoom;
    const RegionDefinition asked = parse(entry);
    const Limits& l = settings_.limits;
    if (asked.maxZoom().value_or(l.maxZoom) > l.maxZoom) {
        throw AreaRefused(std::format("maxzoom must be at most {} for a drawn area", l.maxZoom));
    }
    if (const std::size_t n = asked.area().vertices(); n > l.maxVertices) {
        throw AreaRefused(std::format("the polygon has {} vertices, more than the {} this server takes", n, l.maxVertices));
    }
    if (auto flaw = asked.area().flaw()) throw AreaRefused(*flaw);
    if (const double km2 = asked.area().areaKm2(); km2 > l.maxAreaKm2) {
        throw AreaRefused(std::format("the area is {:.0f} km², more than the {:.0f} km² this server builds", km2, l.maxAreaKm2));
    }
    entry["id"] = std::string(kIdPrefix) + Secrets::hex(Secrets::sha256(asked.canonical())).substr(0, kIdHexDigits);
    return parse(entry);
}

std::string RegionAreas::fingerprint(const RegionDefinition& region) {
    std::string tool;
    {
        const std::scoped_lock lock(mutex_);
        tool = tool_;
    }
    if (tool.empty()) {
        fs::create_directories(settings_.areaDir);
        tool = pmtiles_.version(settings_.areaDir / ".pmtiles-version.log");
        const std::scoped_lock lock(mutex_);
        tool_ = tool;
    }
    return RegionPackageBuilder::fingerprint(region, world_, tool);
}

AreaStatus RegionAreas::request(const json& body) {
    if (!enabled()) throw AreaRefused("drawn areas are off on this server");
    RegionDefinition region = define(body);
    const std::string id = region.id();
    const std::string current = fingerprint(region);
    auto package = store_.load(id);
    const std::scoped_lock lock(mutex_);
    forgetOldFailures();
    const auto known = entries_.find(id);
    if (known != entries_.end() && known->second.status.state != AreaState::Failed) return placed(known->second.status);
    if (package && package->fingerprint == current) {
        if (known != entries_.end()) entries_.erase(known);
        touch(*package);
        return {id, AreaState::Ready, 0, {}, std::move(package)};
    }
    if (queue_.size() >= settings_.limits.maxQueued) {
        throw AreaQueueFull(std::format("{} drawn areas are waiting to be built already", queue_.size()));
    }
    entries_.insert_or_assign(id, Entry{{id, AreaState::Queued, 0, {}, std::move(package)}, clock_.now()});
    queue_.push_back(std::move(region));
    wake_.notify_all();
    return placed(entries_.at(id).status);
}

std::optional<AreaStatus> RegionAreas::status(std::string_view id) const {
    {
        const std::scoped_lock lock(mutex_);
        if (const auto it = entries_.find(id); it != entries_.end()) return placed(it->second.status);
    }
    if (!RegionDefinition::validId(id)) return std::nullopt;
    auto package = store_.load(std::string(id));
    if (!package) return std::nullopt;
    return AreaStatus{std::string(id), AreaState::Ready, 0, {}, std::move(package)};
}

std::optional<RegionPackage> RegionAreas::use(std::string_view id) const {
    if (!RegionDefinition::validId(id)) return std::nullopt;
    auto package = store_.load(std::string(id));
    if (package) touch(*package);
    return package;
}

std::size_t RegionAreas::pending() const {
    const std::scoped_lock lock(mutex_);
    const auto building = std::ranges::count_if(entries_, [](const auto& e) { return e.second.status.state == AreaState::Building; });
    return queue_.size() + static_cast<std::size_t>(building);
}

AreaStatus RegionAreas::placed(AreaStatus status) const {
    if (status.state == AreaState::Queued) {
        const auto it = std::ranges::find(queue_, status.id, &RegionDefinition::id);
        status.position = static_cast<std::size_t>(std::distance(queue_.begin(), it)) + 1;
    }
    return status;
}

void RegionAreas::forgetOldFailures() {
    const TimePoint before = clock_.now() - kFailureMemory;
    std::erase_if(entries_, [before](const auto& e) {
        return e.second.status.state == AreaState::Failed && e.second.since < before;
    });
}

void RegionAreas::run() {
    const std::stop_token stop = stop_.get_token();
    while (!stop.stop_requested()) {
        bool queued = false;
        {
            std::unique_lock lock(mutex_);
            queued = wake_.wait_for(lock, stop, kSweepInterval, [this] { return !queue_.empty(); });
        }
        if (stop.stop_requested()) return;
        if (queued && !buildNext(stop)) return;  // cancelled: the server is stopping
        try {
            sweep();
        } catch (const std::system_error& e) {  // the area directory or its lock file
            log_.error("regions", std::string("removing unused drawn areas failed: ") + e.what());
        }
    }
}

void RegionAreas::drain() {
    while (buildNext(stop_.get_token())) {
        // each call builds one queued area
    }
}

bool RegionAreas::buildNext(const std::stop_token& stop) {
    const std::scoped_lock building(buildMutex_);
    std::optional<RegionDefinition> region;
    {
        const std::scoped_lock lock(mutex_);
        if (queue_.empty()) return false;
        region.emplace(std::move(queue_.front()));
        queue_.pop_front();
        entries_.at(region->id()).status.state = AreaState::Building;
    }
    const std::string id = region->id();
    try {
        fs::create_directories(settings_.areaDir);  // NOSONAR(cppsecurity:S2083): operator configuration
        const FileLock processes(store_.lockFile(), FileLock::Mode::Exclusive);
        const std::string tool = pmtiles_.version(settings_.areaDir / ".pmtiles-version.log");
        const std::string current = RegionPackageBuilder::fingerprint(*region, world_, tool);
        auto package = store_.load(id);
        if (!package || package->fingerprint != current) {  // another server process may have built it
            log_.info("regions", std::format("{}: building the drawn area", id));
            const RegionPackageBuilder builder({world_, pmtiles_, store_, clock_, log_}, stop);
            package = builder.build(*region, current);
            log_.info("regions", std::format("{}: package {} ({} bytes)", id, package->version, package->size));
        }
        touch(*package);
        const std::scoped_lock lock(mutex_);
        tool_ = tool;
        entries_.erase(id);
    } catch (const RegionBuildCancelled&) {
        const std::scoped_lock lock(mutex_);
        entries_.erase(id);
        return false;
    } catch (const RegionError& e) {
        failed(id, e.what());
    } catch (const std::system_error& e) {  // the work directory or the lock file: disk full, permissions
        failed(id, e.what());
    } catch (const json::exception& e) {  // a world style of an unexpected shape
        failed(id, e.what());
    }
    return true;
}

void RegionAreas::failed(const std::string& id, const std::string& why) {
    log_.error("regions", std::format("{}: drawn area not built: {}", id, why));
    auto package = store_.load(id);
    const std::scoped_lock lock(mutex_);
    entries_.insert_or_assign(id, Entry{{id, AreaState::Failed, 0, why, std::move(package)}, clock_.now()});
}

void RegionAreas::touch(const RegionPackage& package) const {
    std::error_code ec;  // a package removed meanwhile is simply not marked
    fs::last_write_time(store_.archive(package), std::chrono::clock_cast<std::chrono::file_clock>(clock_.now()), ec);
}

TimePoint RegionAreas::lastUsed(const RegionPackage& package) const {
    std::error_code ec;
    const auto written = fs::last_write_time(store_.archive(package), ec);
    if (ec) return {};
    return std::chrono::time_point_cast<Clock::duration>(std::chrono::clock_cast<Clock>(written));
}

std::size_t RegionAreas::sweep() {
    if (!enabled()) return 0;
    const std::scoped_lock building(buildMutex_);
    fs::create_directories(settings_.areaDir);  // NOSONAR(cppsecurity:S2083): operator configuration
    const FileLock processes(store_.lockFile(), FileLock::Mode::Exclusive);
    const auto all = store_.loadAll();
    if (!all) return 0;
    std::set<std::string, std::less<>> keep;  // what waits here keeps its older package
    {
        const std::scoped_lock lock(mutex_);
        forgetOldFailures();
        for (const auto& [id, entry] : entries_) {
            if (entry.status.state != AreaState::Failed) keep.insert(id);
        }
    }
    std::vector<std::pair<TimePoint, const RegionPackage*>> used;
    for (const RegionPackage& p : *all) used.emplace_back(lastUsed(p), &p);
    std::ranges::sort(used, std::greater<>(), &std::pair<TimePoint, const RegionPackage*>::first);
    const TimePoint cutoff = clock_.now() - std::chrono::hours(settings_.limits.keepHours);
    std::uintmax_t kept = 0;
    bool full = false;
    for (const auto& [at, p] : used) {
        full = full || kept + p->size > settings_.limits.maxStoredBytes;
        if (at >= cutoff && !full) {
            keep.insert(p->id);
            kept += p->size;
        }
    }
    const std::size_t removed = store_.removeUnused(keep);
    if (removed > 0) log_.info("regions", std::format("removed {} files of unused drawn areas", removed));
    return removed;
}

}  // namespace rfmap
