#pragma once

#include <rfcore/link_budget.hpp>
#include <rfcore/terrain_profile.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfext {

/// ESA WorldCover 10 m class codes.
enum class LandCover : int {
    Tree = 10,
    Shrubland = 20,
    Grassland = 30,
    Cropland = 40,
    BuiltUp = 50,
    BareSparse = 60,
    SnowIce = 70,
    Water = 80,
    Wetland = 90,
    Mangroves = 95,
    MossLichen = 100,
};

const std::vector<LandCover>& allLandCovers();
std::string landCoverName(LandCover c);
std::optional<LandCover> landCoverFromCode(int code);

/// A raster of obstacle heights above ground (canopy height, building height), if configured.
class IObstacleHeightSource {
public:
    virtual ~IObstacleHeightSource() = default;
    [[nodiscard]] virtual std::optional<rfcore::Metres> height(const rfcore::GeoPoint& p) const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

/// What one land cover class adds to the profile.
struct ClassSetting {
    bool enabled = false;
    /// Height used where no raster gives one. Entered by the admin with its source; rfmap ships
    /// no built-in number, because a class alone does not say how tall things are.
    std::optional<rfcore::Metres> height;
    std::string heightSource;
    /// Raster consulted first for this class (canopy for trees, building height for built-up).
    const IObstacleHeightSource* raster = nullptr;
};

/// Puts clutter on the terrain profile as obstacles, class by class. The diffraction model then
/// treats them like terrain: as opaque. Real foliage lets some signal through, so the loss over
/// tree cover is an upper bound; the result says so.
class ClutterConditioner {
public:
    explicit ClutterConditioner(std::map<LandCover, ClassSetting> settings);

    struct Report {
        std::size_t samplesRaised = 0;
        std::map<LandCover, std::size_t> perClass;
        std::size_t samplesWithoutHeight = 0;  ///< enabled class, but neither raster nor setting had a height
        std::size_t samplesWithoutCover = 0;   ///< no land cover data at the sample
        std::vector<std::string> sources;
        std::vector<rfcore::UncertaintyNote> notes() const;
    };

    /// Sets obstacleHeight on interior samples; the two end samples carry the antennas and are left alone.
    Report apply(rfcore::TerrainProfile& profile) const;

private:
    std::map<LandCover, ClassSetting> settings_;
};

}  // namespace rfext
