#pragma once

// The catalog of offline regions: which packages a client can download, and keeping them in step
// with the region definitions and the world data. A worker thread scans at start, every
// regions.rescanMinutes and on an admin reload: it builds the package of each region whose
// definition, world files or pmtiles tool changed, and removes what no region uses any more.
// Requests only read the catalog.

#include "common.hpp"
#include "logger.hpp"
#include "pmtiles_tool.hpp"
#include "region_definition.hpp"
#include "region_package.hpp"
#include "world_tiles.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rfmap {

/// Where a region stands, for the admin.
enum class RegionState { Ready, Building, Failed };

[[nodiscard]] std::string_view regionStateName(RegionState state);

struct RegionStatus {
    std::string id;
    RegionState state = RegionState::Building;
    /// Why the last build failed (Failed only).
    std::string error;
    /// The package clients get: also while a newer one is built, or after its build failed.
    std::optional<RegionPackage> package;
};

class RegionCatalog {
public:
    struct Settings {
        /// The world-wide tiles-builder output the regions are cut from; empty turns regions off.
        std::filesystem::path worldDir;
        /// The JSON file that lists the regions (region_definition.hpp).
        std::filesystem::path definitions;
        std::filesystem::path packageDir;
        /// The go-pmtiles executable.
        std::filesystem::path pmtiles;
        /// Minutes between scans of sourceDir; 0 scans only at start and on reload().
        int rescanMinutes = 10;
        /// false: no worker thread, scans run only when scan() is called (tests).
        bool background = true;
    };

    RegionCatalog(Settings settings, Logger& log, const IClock& clock);
    ~RegionCatalog();
    RegionCatalog(const RegionCatalog&) = delete;
    RegionCatalog& operator=(const RegionCatalog&) = delete;

    [[nodiscard]] bool enabled() const { return !settings_.worldDir.empty(); }
    /// Asks the worker for a scan soon; returns at once.
    void reload();
    /// Scans now on the calling thread, after any scan in progress.
    void scan();

    /// The packages clients can download, sorted by region id.
    [[nodiscard]] std::vector<RegionPackage> packages() const;
    [[nodiscard]] std::optional<RegionPackage> find(std::string_view id) const;
    /// Every region found, ready or not, sorted by id.
    [[nodiscard]] std::vector<RegionStatus> statuses() const;
    /// Scans completed so far.
    [[nodiscard]] std::uint64_t scans() const;
    [[nodiscard]] const RegionPackageStore& store() const { return store_; }

private:
    void run();
    void scanWith(const std::stop_token& stop);
    /// Lists every region found with the package on disk, before any build starts; returns
    /// the regions whose package is missing or older than their files.
    struct Pending {
        const RegionDefinition* region;
        std::string fingerprint;
    };
    std::vector<Pending> prime(const std::vector<RegionDefinition>& found, const std::string& tool);
    /// Builds the package of one region; false when the build was cancelled.
    bool build(const Pending& pending, const RegionPackageBuilder& builder);
    /// Marks `status` failed with `why` and logs it.
    void failed(RegionStatus& status, const std::string& why) const;
    /// Marks every region failed with `why`, keeping the packages they have.
    void failAll(const std::vector<RegionDefinition>& found, const std::string& why);
    void set(RegionStatus status);
    /// Drops the regions that are no longer in `found`.
    void retain(const std::vector<RegionDefinition>& found);
    /// The definitions file as it is now; logs what it leaves out.
    [[nodiscard]] std::vector<RegionDefinition> definitions() const;

    Settings settings_;
    Logger& log_;
    const IClock& clock_;
    RegionPackageStore store_;
    WorldTiles world_;
    PmtilesTool pmtiles_;

    mutable std::mutex mutex_;  ///< guards regions_ and scans_
    std::map<std::string, RegionStatus, std::less<>> regions_;
    std::uint64_t scans_ = 0;

    std::mutex scanMutex_;  ///< one scan at a time in this process; FileLock keeps processes apart
    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    bool pending_ = false;  ///< under wakeMutex_
    std::stop_source stop_;
    std::jthread worker_;  ///< declared last: started after everything it uses, joined first
};

}  // namespace rfmap
