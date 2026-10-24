#pragma once

// fpv.airframe: whether a fixed-wing drone can fly the route as planned. The plane climbs and
// descends at limited angles, turns on a radius and must stay above its stall speed; a
// multirotor has none of these limits.

#include <fpv/airframe.hpp>
#include <fpv/route.hpp>

#include <optional>
#include <string>
#include <vector>

namespace fpv::airframe {

/// One point of the route's vertical profile, at the DEM's step along each leg.
struct ProfilePoint {
    double alongM;       ///< distance along the whole route
    LatLon position;
    std::size_t leg;
    double plannedMslM;  ///< where the plan puts the plane
    double groundMslM;
    double obstacleM;    ///< clutter on top of the ground
    bool returnLeg;      ///< on the way back to the pilot, where the plane circles down to land
};

/// The route's vertical profile: the planned altitude over the ground under it.
class VerticalProfile {
public:
    /// Throws MissingTerrain where the DEM has no ground under the route.
    static std::vector<ProfilePoint> of(const Route& route, const IWorld& world);
};

/// Where the plane is below the planned height because it cannot climb as steeply as planned.
struct Shortfall {
    ProfilePoint at;
    double belowPlanM;
};

/// A waypoint the plane reaches below its planned height, and the extra distance the climb needs.
struct LateArrival {
    std::size_t leg;  ///< the leg that ends at the waypoint
    double belowPlanM;
    double extraClimbM;  ///< more horizontal distance at the steepest climb to reach the height
};

/// A waypoint the plane reaches above its planned height because it cannot descend as steeply.
struct HighArrival {
    std::size_t leg;  ///< the leg that ends at the waypoint
    double abovePlanM;
    double extraDescentM;  ///< more horizontal distance at the steepest descent to get down
};

struct ClimbResult {
    std::optional<Shortfall> worst;          ///< the largest gap below the plan, over the tolerance
    std::optional<Shortfall> terrain;        ///< the first point closer to terrain than the clearance the plan keeps
    double terrainGapM = 0.0;                ///< the plane's height over terrain plus clutter there
    std::vector<LateArrival> arrivals;
    std::vector<HighArrival> highArrivals;  ///< not the landing at the pilot, which circles down
};

/// Flies the profile as an autopilot holding the planned altitude would: it follows the plan
/// where it can and otherwise climbs or descends at the steepest angle the envelope allows.
class ClimbSimulator {
public:
    ClimbSimulator(const Airframe& airframe, double clearanceM, double toleranceM)
        : airframe_(airframe), clearance_(clearanceM), tolerance_(toleranceM) {}

    [[nodiscard]] ClimbResult fly(const std::vector<ProfilePoint>& profile) const;

private:
    void checkTerrain(const ProfilePoint& p, double achievedMsl, ClimbResult& out) const;
    void recordArrival(const ProfilePoint& p, double belowPlan, ClimbResult& out) const;

    const Airframe& airframe_;
    double clearance_;
    double tolerance_;
};

/// The turn at a waypoint between two legs, flown as a fly-by on the turn radius.
struct Turn {
    std::size_t legIn;       ///< the leg that ends at the waypoint
    double headingChangeDeg;
    double radiusM;
    double leadM;            ///< where the turn starts before the waypoint
    double insideM;          ///< how far inside the waypoint the plane passes
    bool uTurn;              ///< 150° or more: the plane flies past and swings out by 2 r
    bool fits;               ///< the turn ends before half of each leg is used up
};

class TurnPlanner {
public:
    static constexpr double kUTurnDeg = 150.0;

    /// `groundSpeedMps`: the fastest ground speed in the turn (airspeed plus the wind), which
    /// sets the radius over the ground.
    TurnPlanner(const Airframe& airframe, double groundSpeedMps) : airframe_(airframe), speed_(groundSpeedMps) {}

    [[nodiscard]] std::vector<Turn> turns(const Route& route, const IWorld& world) const;

private:
    [[nodiscard]] Turn turnAt(const Leg& in, const Leg& out, double inAzimuthDeg) const;

    const Airframe& airframe_;
    double speed_;
};

/// route.analyze for a fixed-wing or multirotor drone profile; `world` answers geodesy and terrain.
json analyze(const json& request, const IWorld& world, const json& drone);

}  // namespace fpv::airframe
