#pragma once

#include "rfcore/earth_model.hpp"
#include "rfcore/terrain_profile.hpp"
#include "rfcore/units.hpp"

#include <vector>

namespace rfcore {

/// A point in the plane of the great circle through both terminals, in metres.
/// The origin is the ground under the first terminal; y points up there.
struct PlanePoint {
    double x;
    double y;
};

/// Where a profile point sits relative to the direct ray between the antennas.
struct RayRelation {
    Metres d1;           ///< Distance along the ray from the first antenna to the foot of the perpendicular.
    Metres d2;           ///< Same from the second antenna.
    Metres heightAbove;  ///< Signed perpendicular distance; positive when the point is above the ray.
};

/// Exact geometry of a path in the effective-earth model: every profile point is placed on
/// a circle of radius k·R at its own height, and the ray between the antennas is a straight
/// line. No parabolic "earth bulge" approximation is used.
class PathGeometry {
public:
    PathGeometry(const TerrainProfile& profile, const EarthModel& earth, Metres antennaAglStart,
                 Metres antennaAglEnd);

    [[nodiscard]] Metres effectiveRadius() const { return effectiveRadius_; }
    [[nodiscard]] PlanePoint antennaStart() const { return start_; }
    [[nodiscard]] PlanePoint antennaEnd() const { return end_; }
    /// Straight-line distance between the antennas, used for free-space loss.
    [[nodiscard]] Metres rayLength() const;
    /// Profile tops (ground plus clutter) in the plane, same order as the profile samples.
    [[nodiscard]] const std::vector<PlanePoint>& tops() const { return tops_; }

    [[nodiscard]] PlanePoint toPlane(Metres distanceAlongGround, Metres heightMsl) const;
    [[nodiscard]] RayRelation relate(const PlanePoint& p) const;

private:
    Metres effectiveRadius_;
    Metres baseHeight_;  ///< Ground MSL at the first terminal; the plane origin.
    PlanePoint start_{};
    PlanePoint end_{};
    std::vector<PlanePoint> tops_;
};

}  // namespace rfcore
