#pragma once

#include "common.hpp"
#include "raster.hpp"

#include <optional>

namespace rfmap {

/// mission.altitudeRef for the route calculation: where the drone is between two route points.
/// Between two points the height changes linearly with distance along the leg in the mission's
/// reference: above the ground at the pilot ("launch", a barometric altitude from take-off, the
/// waypoints' altM) or above the ground under the drone ("agl", the waypoints' aglM). A mission
/// without the field is an AGL mission. The pilot's height is its antenna above the ground in both.
class RouteAltitude {
public:
    RouteAltitude(const json& mission, const rfcore::IElevationSource& dem);

    [[nodiscard]] bool followsTerrain() const { return !launch_; }

    /// A route point's height in the mission's reference; `isPilot` for the pilot's own position.
    [[nodiscard]] double heightOf(const json& point, bool isPilot) const;

    /// The drone's height above the ground at `position` for a height in the mission's reference,
    /// negative when the plan puts it below the ground. Nothing when the DEM has no ground there
    /// or at the pilot.
    [[nodiscard]] std::optional<double> aglAt(const rfcore::GeoPoint& position, double heightM) const;

private:
    const rfcore::IElevationSource& dem_;
    bool launch_;
    std::optional<double> launchGround_;
};

}  // namespace rfmap
