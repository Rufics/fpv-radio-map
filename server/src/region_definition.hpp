#pragma once

// Offline regions, the operator's side: which pieces of the world map clients may download.
// The server keeps one world-wide set of map layers (tiles-builder output, world_tiles.hpp) and
// cuts each region out of it by a bounding box or a polygon such as an administrative boundary
// (region_package.hpp). The regions are listed in a JSON file (regions.definitions):
//
//   {"regions": [
//     {"id": "kharkiv", "name": "Kharkiv oblast", "bbox": [34.8, 48.3, 37.0, 50.5]},
//     {"id": "dnipro", "name": "Dnipro oblast", "boundary": "boundaries/dnipro.geojson",
//      "maxzoom": 14, "layers": ["basemap", "hillshade", "terrain", "contours"],
//      "glyphRanges": ["0-255", "1024-1279"]}
//   ]}

#include "common.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// A region that cannot be read or packaged. The catalog logs it and keeps the region's last
/// good package, if any.
class RegionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A build that was asked to stop (the server is stopping); nothing of it is kept.
class RegionBuildCancelled : public RegionError {
public:
    RegionBuildCancelled() : RegionError("region build cancelled") {}
};

/// A WGS84 bounding box in degrees, as the API exchanges it: [minLon, minLat, maxLon, maxLat].
class GeoBox {
public:
    GeoBox(double minLon, double minLat, double maxLon, double maxLat);

    /// A box from a JSON array of four numbers; nothing when it is not one or not a valid box.
    [[nodiscard]] static std::optional<GeoBox> fromJson(const json& value);
    [[nodiscard]] json toJson() const;
    /// "minLon,minLat,maxLon,maxLat", as `pmtiles extract --bbox` takes it.
    [[nodiscard]] std::string toArgument() const;
    [[nodiscard]] double centreLon() const { return (minLon_ + maxLon_) / 2.0; }
    [[nodiscard]] double centreLat() const { return (minLat_ + maxLat_) / 2.0; }
    [[nodiscard]] double minLon() const { return minLon_; }
    [[nodiscard]] double minLat() const { return minLat_; }
    [[nodiscard]] double maxLon() const { return maxLon_; }
    [[nodiscard]] double maxLat() const { return maxLat_; }

private:
    double minLon_;
    double minLat_;
    double maxLon_;
    double maxLat_;
};

/// The area a region covers: a box, or a GeoJSON Polygon or MultiPolygon with the box around it.
class RegionArea {
public:
    explicit RegionArea(const GeoBox& box) : box_(box) {}
    /// From a GeoJSON geometry, Feature or FeatureCollection (its features' polygons together);
    /// RegionError when it holds no polygon with valid coordinates.
    [[nodiscard]] static RegionArea fromGeoJson(const json& geojson);

    [[nodiscard]] const GeoBox& box() const { return box_; }
    /// The polygon as a GeoJSON MultiPolygon geometry; nothing for a plain box.
    [[nodiscard]] const std::optional<json>& polygon() const { return polygon_; }
    /// The area on the Earth in km²: the polygon's, holes taken out, or the box's.
    [[nodiscard]] double areaKm2() const;
    /// The positions of every ring of the polygon; 4 for a box.
    [[nodiscard]] std::size_t vertices() const;

private:
    RegionArea(const GeoBox& box, json polygon) : box_(box), polygon_(std::move(polygon)) {}
    /// The rings of a Polygon or MultiPolygon appended to `polygons`, checked.
    static void collect(const json& geometry, json& polygons);
    /// The box around every position of `polygons`.
    [[nodiscard]] static GeoBox boxAround(const json& polygons);

    GeoBox box_;
    std::optional<json> polygon_;
};

/// One region of the definitions file, checked.
class RegionDefinition {
public:
    /// The map layers a package may carry, as world_tiles.hpp names them.
    static constexpr auto kLayers = std::to_array<std::string_view>({"basemap", "satellite", "hillshade", "terrain", "contours"});

    /// Reads one entry; `base` is the directory boundary files are relative to, and a boundary
    /// file must lie under it. RegionError names what is wrong.
    RegionDefinition(const json& entry, const std::filesystem::path& base);

    /// Region ids: lower-case letters, digits and dashes, 1 to 64 long, no dash at either end.
    [[nodiscard]] static bool validId(std::string_view id);

    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const RegionArea& area() const { return area_; }
    [[nodiscard]] const std::optional<int>& maxZoom() const { return maxZoom_; }
    /// The layers asked for; empty = every layer the world has.
    [[nodiscard]] const std::vector<std::string>& layers() const { return layers_; }
    /// The glyph ranges ("0-255") to carry; empty = all of them.
    [[nodiscard]] const std::vector<std::string>& glyphRanges() const { return glyphRanges_; }
    /// Everything the package depends on in this entry, as canonical JSON, for the fingerprint.
    [[nodiscard]] std::string canonical() const;

private:
    [[nodiscard]] static RegionArea readArea(const json& entry, const std::filesystem::path& base);
    [[nodiscard]] static std::filesystem::path boundaryFile(const std::string& name, const std::filesystem::path& base);
    void readOptions(const json& entry);

    std::string id_;
    std::string name_;
    RegionArea area_;
    std::optional<int> maxZoom_;
    std::vector<std::string> layers_;
    std::vector<std::string> glyphRanges_;
};

/// The definitions file, read once.
class RegionDefinitions {
public:
    /// An entry left out, and why.
    struct Skip {
        std::string id;  ///< "?" when the entry has no string id
        std::string why;
    };

    /// Reads `file`. A missing file defines no regions; a file that cannot be read or is not
    /// JSON is RegionError. An entry that is wrong is left out and listed in skipped(), and so
    /// is a second entry with an id already taken.
    explicit RegionDefinitions(const std::filesystem::path& file);

    /// The regions, sorted by id.
    [[nodiscard]] const std::vector<RegionDefinition>& regions() const { return regions_; }
    [[nodiscard]] const std::vector<Skip>& skipped() const { return skipped_; }

private:
    std::vector<RegionDefinition> regions_;
    std::vector<Skip> skipped_;
};

}  // namespace rfmap
