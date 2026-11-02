#pragma once

// Offline region packages. A package is everything MapLibre needs to show a region without a
// network, in one zip whose entries are stored, not compressed (PMTiles and PNG are compressed
// already):
//
//   manifest.json                  format, id, name, version, bbox, layers, and every file with
//                                  its size and SHA-256
//   style.json                     the world style, its URLs rewritten to paths in the package
//   tiles/<layer>.pmtiles          each layer cut out of the world archive (pmtiles_tool.hpp)
//   glyphs/<fontstack>/<range>.pbf the fonts the style uses, in the ranges the region asks for
//   sprites/sprite[@2x].{json,png} the icons, when the world has them
//
// The version of a package is the SHA-256 of its manifest's file list, so the same files make
// the same version however often the package is built, and a client downloads again only when
// something it would get changed. It is also the package's entity tag.
//
// regions.packageDir keeps each package as <id>-<version>.zip next to <id>.json, its record, and
// <id>-<preview version>.png, its preview, named by its own SHA-256. A new version is written under a temporary name and only then
// recorded, so no client ever gets a half-written package.

#include "common.hpp"
#include "logger.hpp"
#include "pmtiles_tool.hpp"
#include "region_definition.hpp"
#include "world_tiles.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// A built package as its record keeps it and the catalog lists it.
struct RegionPackage {
    std::string id;
    std::string name;
    GeoBox bbox{-180, -90, 180, 90};
    std::optional<int> maxZoom;
    std::vector<std::string> layers;
    /// Bytes of the zip.
    std::uintmax_t size = 0;
    /// The package version: SHA-256 (hex) of the manifest.
    std::string version;
    /// SHA-256 (hex) of the zip, for checking a download.
    std::string sha256;
    std::string builtAt;
    /// What it was built from (definition, world files, tool version); unchanged = no rebuild.
    std::string fingerprint;
    bool preview = false;
    /// The preview's own version: SHA-256 (hex) of the PNG. A new DEM redraws the preview without
    /// changing the package, and the preview URL changes with it.
    std::string previewVersion;

    [[nodiscard]] json toRecord() const;
    /// A record as toRecord wrote it; nothing when it is damaged or of another layout.
    [[nodiscard]] static std::optional<RegionPackage> fromRecord(const json& record);
    /// The strong entity tag: the version in quotes.
    [[nodiscard]] std::string etag() const { return "\"" + version + "\""; }
    /// The strong entity tag of the preview.
    [[nodiscard]] std::string previewEtag() const { return "\"" + previewVersion + "\""; }
};

/// The package directory: packages, previews, their records and the builds in progress.
class RegionPackageStore {
public:
    explicit RegionPackageStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
    /// The recorded package of `id`, if its record is sound and its zip is there.
    [[nodiscard]] std::optional<RegionPackage> load(const std::string& id) const;
    /// Every sound recorded package in the directory, whatever regions are defined now;
    /// nothing when the directory cannot be listed.
    [[nodiscard]] std::optional<std::vector<RegionPackage>> loadAll() const;
    /// Moves the files of a finished build into place and records `package` as current.
    void commit(const RegionPackage& package) const;
    [[nodiscard]] std::filesystem::path archive(const RegionPackage& package) const;
    [[nodiscard]] std::filesystem::path preview(const RegionPackage& package) const;
    /// The directory a build of `id` works in; the build removes it when it ends.
    [[nodiscard]] std::filesystem::path workDir(const std::string& id) const;
    /// Where a build writes its zip and preview before it knows the version.
    [[nodiscard]] std::filesystem::path tempArchive(const std::string& id) const;
    [[nodiscard]] std::filesystem::path tempPreview(const std::string& id) const;
    /// Removes what no current package of the regions `current` uses: older versions, regions
    /// no longer defined, files of builds that never finished. Returns how many entries went.
    std::size_t removeUnused(const std::set<std::string, std::less<>>& current) const;
    /// The lock file that keeps the builds of several server processes apart.
    [[nodiscard]] std::filesystem::path lockFile() const { return dir_ / ".rfmap-regions.lock"; }

private:
    [[nodiscard]] std::filesystem::path record(const std::string& id) const { return dir_ / (id + ".json"); }
    /// The names of the entries the current packages of `current` use.
    [[nodiscard]] std::set<std::string, std::less<>> inUse(const std::set<std::string, std::less<>>& current) const;

    std::filesystem::path dir_;
};

/// The world style made into the style of a package.
class RegionStyle {
public:
    /// `style` with tile URLs to tiles/, glyphs to glyphs/, the sprite to sprites/ (or none),
    /// the sources of layers the package lacks removed with their style layers, a raster-dem
    /// source for the terrain layer, and the map centred on the region.
    [[nodiscard]] static json rewrite(json style, const RegionPackage& package, bool sprites);
    /// The fontstacks the style's symbol layers name, e.g. "Noto Sans Regular".
    [[nodiscard]] static std::vector<std::string> fontstacks(const json& style);

private:
    /// Points each pmtiles source at the package or drops it; returns the dropped source names.
    static std::set<std::string, std::less<>> rewriteSources(json& style, const std::vector<std::string>& layers);
};

/// Draws the preview of a region: a small grey hillshade of the world DEM over its box, as a PNG.
class RegionPreview {
public:
    static constexpr int kWidth = 256;

    /// RegionError when GDAL cannot (no DEM over the box, for one).
    static void render(const std::filesystem::path& dem, const GeoBox& box, const std::filesystem::path& png);
};

/// Builds the package of one region from the world data into a store.
class RegionPackageBuilder {
public:
    static constexpr std::string_view kManifest = "manifest.json";
    static constexpr std::string_view kStyle = "style.json";
    static constexpr int kFormat = 1;

    struct Tools {
        const WorldTiles& world;
        const PmtilesTool& pmtiles;
        const RegionPackageStore& store;
        const IClock& clock;
        Logger& log;
    };

    RegionPackageBuilder(const Tools& tools, std::stop_token stop) : t_(tools), stop_(std::move(stop)) {}

    /// Builds and records the package of `region`; RegionError when it cannot, and the region's
    /// earlier package then stays current. `fingerprint` is recorded with it.
    RegionPackage build(const RegionDefinition& region, const std::string& fingerprint) const;

    /// The layers a package of `region` carries: those it asks for that the world has.
    [[nodiscard]] static std::vector<std::string> layersOf(const RegionDefinition& region, const WorldTiles& world);

    /// One file of a package: its name inside, and where its bytes come from.
    struct Entry {
        std::string name;
        std::filesystem::path file;  ///< empty for bytes made in memory
        std::string bytes;
    };

private:
    /// `file` copied into the work directory as package entry `name`.
    [[nodiscard]] Entry snapshot(const RegionDefinition& region, std::string name, const std::filesystem::path& file) const;
    /// Cuts every layer into the work directory; returns their entries.
    std::vector<Entry> extractLayers(const RegionDefinition& region, const RegionPackage& package) const;
    /// The glyph and sprite entries for `style`.
    [[nodiscard]] std::vector<Entry> styleAssets(const RegionDefinition& region, const json& style) const;
    /// The manifest of a package of `entries`; sets package.version.
    [[nodiscard]] json manifest(RegionPackage& package, const std::vector<Entry>& entries) const;
    /// Writes the zip; returns its size and SHA-256.
    [[nodiscard]] std::pair<std::uintmax_t, std::string> writeZip(const json& manifest, const std::vector<Entry>& entries,
                                                                const std::string& id) const;
    /// Draws the preview and sets its version; false without a DEM or when it fails (logged).
    [[nodiscard]] bool writePreview(RegionPackage& package) const;

    Tools t_;
    std::stop_token stop_;
};

}  // namespace rfmap
