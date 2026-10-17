#pragma once

#include "rfcore/geodesy.hpp"
#include "rfcore/units.hpp"

#include <optional>
#include <string>
#include <vector>

namespace rfcore {

/// One elevation value with the error the data source states for it.
struct ElevationSample {
    Metres heightMsl;     ///< Height above mean sea level (the DEM's vertical datum).
    Metres uncertainty;   ///< Stated vertical error of the source (for example LE90), not ours.
};

/// Where terrain heights come from. Implemented outside the core (GDAL reader of a COG,
/// a test fixture, a user's LiDAR file); the core only asks for points.
class IElevationSource {
public:
    virtual ~IElevationSource() = default;
    /// Height at a point, or nothing when the point is outside the loaded data.
    /// Implementations must never invent a value for a missing point.
    [[nodiscard]] virtual std::optional<ElevationSample> elevation(const GeoPoint& point) const = 0;
    /// Native grid spacing; profiles are sampled at this step.
    [[nodiscard]] virtual Metres nativeResolution() const = 0;
    /// Name and version shown in results, for example "Copernicus DEM GLO-30 2023_1".
    [[nodiscard]] virtual std::string name() const = 0;
};

struct ProfileSample {
    Metres distance;        ///< Geodesic distance from the start of the path.
    Metres groundMsl;       ///< Terrain height above sea level.
    Metres obstacleHeight;  ///< Clutter (trees, buildings) on top of the ground; 0 without rf.clutter.
    Metres uncertainty;     ///< Source vertical error at this sample.
    GeoPoint position;

    [[nodiscard]] Metres topMsl() const { return groundMsl + obstacleHeight; }
};

/// A terrain cross-section along a geodesic. Complete only when every sample had data.
class TerrainProfile {
public:
    TerrainProfile(std::vector<ProfileSample> samples, std::string sourceName, double azimuthDeg,
                   double midLatitudeDeg);

    [[nodiscard]] const std::vector<ProfileSample>& samples() const { return samples_; }
    [[nodiscard]] Metres length() const { return samples_.back().distance; }
    [[nodiscard]] const std::string& sourceName() const { return sourceName_; }
    [[nodiscard]] double azimuthDeg() const { return azimuthDeg_; }
    [[nodiscard]] double midLatitudeDeg() const { return midLatitudeDeg_; }

    /// Mutable access for extensions that add clutter heights.
    [[nodiscard]] std::vector<ProfileSample>& mutableSamples() { return samples_; }

private:
    std::vector<ProfileSample> samples_;
    std::string sourceName_;
    double azimuthDeg_;
    double midLatitudeDeg_;
};

/// A stretch of the path with no elevation data.
struct MissingSpan {
    Metres from;
    Metres to;
};

/// Either a complete profile or the list of spans that have no data. We never fill gaps.
struct ProfileResult {
    std::optional<TerrainProfile> profile;
    std::vector<MissingSpan> missing;
};

class ProfileBuilder {
public:
    ProfileBuilder(const IElevationSource& source, Geodesy geodesy = {});

    /// Samples the terrain from `from` to `to` at the source's native resolution
    /// (or `step` when given and finer).
    [[nodiscard]] ProfileResult build(const GeoPoint& from, const GeoPoint& to,
                                      std::optional<Metres> step = std::nullopt) const;

private:
    const IElevationSource& source_;
    Geodesy geodesy_;
};

}  // namespace rfcore
