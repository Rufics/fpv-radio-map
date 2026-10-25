#pragma once

#include "corridor.hpp"

#include <optional>

namespace fpv::altitude {

/// The climb assistant's options, with the schema's defaults.
struct ClimbSettings {
    bool enabled = true;
    double climbRateMps = 0.0;   ///< 0 = the drone's measured climb rate, else kDefaultClimbMps
    double descentRateMps = 4.0;
    double speedMps = 0.0;       ///< 0 = the drone's cruise airspeed, else kDefaultSpeedMps
    double heightStepM = 5.0;
    double fitToleranceM = 5.0;

    static constexpr double kDefaultClimbMps = 3.0;
    static constexpr double kDefaultSpeedMps = 15.0;

    static ClimbSettings fromOptions(const json& options);
};

struct ClimbAdvice {
    json data;                     ///< data.climb: the plan and every strategy, each with its fix
    json reasons = json::array();  ///< climbs the drone cannot make, and a better profile
};

/// The climb assistant over a built corridor: the plan's climb against the energy-optimal profile
/// and the simpler strategies, each turned into waypoints and offered as a fix. `drone` is the
/// library item of the mission's drone, when it has one; without it energy is not estimated.
/// `weather` is the forecast it flies in (null: still air at standard conditions).
ClimbAdvice climbAssist(const json& request, const Route& route, const IWorld& world,
                        const std::vector<CorridorPoint>& pts, const ClimbSettings& set,
                        const std::optional<json>& drone, const json& weather);

}  // namespace fpv::altitude
