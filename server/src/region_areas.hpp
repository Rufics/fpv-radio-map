#pragma once

// Areas a user draws on the map: the package of a polygon built on request, cut from the same
// world data as the operator's regions (region_catalog.hpp) by the same builder.
//
// A request names a polygon, and optionally a name, layers and a highest zoom. The server checks
// it against regions.areas (area, vertices, zoom), and the id of the area is a hash of what was
// asked, so the same polygon asked twice is one package, built once. Builds wait in a queue of
// regions.areas.maxQueued and run one at a time on a worker thread; asking for an area whose
// package is current answers at once.
//
// Packages live in regions.areaDir, laid out as in regions.packageDir. Each counts as used when
// it is asked for or downloaded; one unused for regions.areas.keepHours is removed, and so are the
// least recently used ones once the directory holds more than regions.areas.maxStoredMb.

#include "common.hpp"
#include "logger.hpp"
#include "pmtiles_tool.hpp"
#include "region_definition.hpp"
#include "region_package.hpp"
#include "world_tiles.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace rfmap {

/// A drawn area the server does not build: outside the limits, or not an area at all.
class AreaRefused : public RegionError {
public:
    using RegionError::RegionError;
};

/// Every place in the build queue is taken; the client asks again later.
class AreaQueueFull : public RegionError {
public:
    using RegionError::RegionError;
};

enum class AreaState { Queued, Building, Ready, Failed };

[[nodiscard]] std::string_view areaStateName(AreaState state);

struct AreaStatus {
    std::string id;
    AreaState state = AreaState::Queued;
    /// Place in the queue, 1 = next to build (Queued only).
    std::size_t position = 0;
    /// Why the build failed (Failed only).
    std::string error;
    /// The package: also an older one while a newer one is built.
    std::optional<RegionPackage> package;
};

class RegionAreas {
public:
    static constexpr std::string_view kIdPrefix = "area-";
    static constexpr std::string_view kDefaultName = "Drawn area";

    struct Limits {
        /// The largest area built, km²; 0 turns drawn areas off.
        double maxAreaKm2 = 2500.0;
        /// Positions in all rings of the polygon together.
        std::size_t maxVertices = 1000;
        /// The highest zoom a package holds; also the zoom of a request that names none.
        int maxZoom = 14;
        /// Builds that may wait; a request beyond them gets 503.
        std::size_t maxQueued = 8;
        /// A package nobody asked for or downloaded this long is removed.
        int keepHours = 168;
        /// The packages kept together, bytes; the least recently used go first.
        std::uintmax_t maxStoredBytes = std::uintmax_t{20} << 30;
    };

    struct Settings {
        /// The world-wide tiles-builder output; empty turns drawn areas off.
        std::filesystem::path worldDir;
        /// Where the packages of drawn areas are kept; not regions.packageDir.
        std::filesystem::path areaDir;
        std::filesystem::path pmtiles;
        Limits limits;
        /// false: no worker thread, builds run only when drain() is called (tests).
        bool background = true;
    };

    RegionAreas(Settings settings, Logger& log, const IClock& clock);
    ~RegionAreas();
    RegionAreas(const RegionAreas&) = delete;
    RegionAreas& operator=(const RegionAreas&) = delete;

    [[nodiscard]] bool enabled() const { return !settings_.worldDir.empty() && settings_.limits.maxAreaKm2 > 0.0; }
    [[nodiscard]] const Limits& limits() const { return settings_.limits; }
    [[nodiscard]] const RegionPackageStore& store() const { return store_; }

    /// The region a request body asks for: {"geometry": GeoJSON Polygon or MultiPolygon (or a
    /// Feature or FeatureCollection of them), "name"?, "layers"?, "maxzoom"?}. AreaRefused says
    /// what is wrong or over the limits.
    [[nodiscard]] RegionDefinition define(const json& body) const;
    /// The area of `body`: ready when its package is current, otherwise queued for a build.
    /// AreaRefused as define(); AreaQueueFull when the queue is full; RegionError when the
    /// world data cannot be read.
    AreaStatus request(const json& body);
    /// The status of area `id`; nothing for an id never asked for or long removed.
    [[nodiscard]] std::optional<AreaStatus> status(std::string_view id) const;
    /// The current package of area `id`, counted as used now.
    std::optional<RegionPackage> use(std::string_view id) const;
    /// Builds waiting and building.
    [[nodiscard]] std::size_t pending() const;

    /// Builds every queued area on the calling thread (tests, background = false).
    void drain();
    /// Removes the packages unused for keepHours, then the least recently used beyond
    /// maxStoredBytes. Returns how many directory entries went.
    std::size_t sweep();

private:
    /// A queued, building or failed area; ready ones are only on disk.
    struct Entry {
        AreaStatus status;
        TimePoint since;
    };

    void run();
    /// Builds the next queued area; false when there is none or the build was cancelled.
    bool buildNext(const std::stop_token& stop);
    /// The fingerprint of `region` with the tool version known now.
    [[nodiscard]] std::string fingerprint(const RegionDefinition& region);
    /// Records why the build of `id` failed.
    void failed(const std::string& id, const std::string& why);
    /// Marks the package as used now: its zip's modification time.
    void touch(const RegionPackage& package) const;
    [[nodiscard]] TimePoint lastUsed(const RegionPackage& package) const;
    /// `status` with its place in the queue; under mutex_.
    [[nodiscard]] AreaStatus placed(AreaStatus status) const;
    /// Forgets failed builds older than an hour; under mutex_.
    void forgetOldFailures();

    Settings settings_;
    Logger& log_;
    const IClock& clock_;
    RegionPackageStore store_;
    WorldTiles world_;
    PmtilesTool pmtiles_;

    mutable std::mutex mutex_;  ///< guards queue_, entries_ and tool_
    std::deque<RegionDefinition> queue_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::string tool_;  ///< the pmtiles version line, as the last build read it

    std::mutex buildMutex_;  ///< one build or sweep at a time in this process
    std::condition_variable_any wake_;  // NOSONAR(cpp:S8467): only the _any variant waits on a stop_token
    std::stop_source stop_;
    std::jthread worker_;  ///< declared last: started after everything it uses, joined first
};

}  // namespace rfmap
