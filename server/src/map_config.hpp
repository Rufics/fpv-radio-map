#pragma once

#include "common.hpp"
#include "config.hpp"
#include "raster.hpp"

namespace rfmap {

/// GET /config/map: where the client finds its tiles, fonts and credits. Built from the
/// tiles-builder manifest next to the DEM and the operator's `map` section.
class MapConfig {
public:
    MapConfig(const Config& config, const DemSource* dem) : config_(config), dem_(dem) {}

    [[nodiscard]] json build() const;

private:
    /// Drops the layers the manifest does not list, so the client shows no view it cannot load.
    static void dropUnbuiltLayers(json& map, const std::filesystem::path& dem);

    const Config& config_;
    const DemSource* dem_;
};

}  // namespace rfmap
