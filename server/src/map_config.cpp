#include "map_config.hpp"

#include "tiles_manifest.hpp"

namespace rfmap {

void MapConfig::dropUnbuiltLayers(json& map, const TilesManifest& manifest) {
    const auto files = manifest.files();
    if (!files) return;  // without a manifest every layer stays
    for (const char* layer : {"basemap", "satellite", "hillshade", "contours"}) {
        if (!files->contains(std::string(layer) + ".pmtiles")) map.erase(layer);
    }
    if (!files->contains("terrain.pmtiles")) map.erase("dem");
}

json MapConfig::build() const {
    // tools/tiles-builder output names, served under /tiles by the web container. A `map` object in
    // the config is merged over these (RFC 7386), so an operator changes only what differs.
    json out = {{"basemap", {{"pmtiles", "/tiles/basemap.pmtiles"}}},
                {"satellite", {{"pmtiles", "/tiles/satellite.pmtiles"}}},
                {"hillshade", {{"pmtiles", "/tiles/hillshade.pmtiles"}}},
                {"contours", {{"pmtiles", "/tiles/contours.pmtiles"}, {"sourceLayer", "contours"}}},
                // Heights for the 3D view, Terrarium-encoded (tiles-builder terrain.pmtiles).
                {"dem", {{"pmtiles", "/tiles/terrain.pmtiles"}, {"encoding", "terrarium"}}},
                {"glyphs", "/tiles/fonts/{fontstack}/{range}.pbf"},
                {"zoom", 11}};
    // The satellite credit is the operator's: it comes from the manifest the tiles-builder wrote.
    if (!config_.data.dem.empty()) {
        const TilesManifest manifest(config_.data.dem);  // read once: credits and layers agree
        if (const auto attributions = manifest.attributions(); !attributions.empty()) {
            out["attribution"] = attributions;
        }
        dropUnbuiltLayers(out, manifest);
    }
    if (dem_) {
        const rfcore::GeoPoint c = dem_->raster().centre();
        out["center"] = {c.longitudeDeg, c.latitudeDeg};
    }
    if (config_.raw.contains("map")) out.merge_patch(config_.raw.at("map"));
    return out;
}

}  // namespace rfmap
