#pragma once

#include "corridor.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fpv::altitude {

/// What the drone can do between two corridor samples, from its climb and descent rates and the
/// ground speed on that stretch.
struct Segment {
    double maxRiseM;  ///< highest climb over the segment; infinite in the take-off zone
    double maxDropM;  ///< deepest descent over the segment; infinite in the take-off zone
    double groundSpeedMps;
    double climbMps;    ///< the drone's climb and descent rates, for the time a steep stretch takes
    double descentMps;
};

/// A height profile along the corridor samples, above mean sea level.
struct ClimbProfile {
    std::string id;
    std::vector<double> mslM;
    bool withinClimbRate = true;  ///< false when the drone cannot follow it outside the take-off zone
};

/// How a profile does, the same for the plan and every strategy.
struct ProfileStats {
    double riseM = 0.0;             ///< total climb, from the pilot on
    double minClearanceM = 0.0;     ///< lowest height over the floor, outside the take-off zone
    double minClearanceAtM = 0.0;
    double overCeilingM = 0.0;      ///< highest height over the ceiling where the floor is under it; 0 inside
    double steepestClimbMps = 0.0;  ///< outside the take-off zone, at the ground speed there
    double steepestClimbAtM = 0.0;
    double timeS = 0.0;             ///< a stretch takes as long as its distance or its climb or descent does
};

/// The lowest-energy way through the corridor and the simpler strategies to compare it with.
/// Climbing costs energy and descending gives nothing back, so the energy-optimal profile is the
/// one with the least total climb that stays between floor and ceiling within the drone's climb
/// and descent rates; among those it keeps the lowest heights.
class ClimbPlanner {
public:
    /// `segments[i]` is the stretch from `pts[i]` to `pts[i + 1]`. The profile starts at the plan's
    /// height at the pilot, and ends at it too when the route returns there.
    ClimbPlanner(const std::vector<CorridorPoint>& pts, std::vector<Segment> segments, bool returns, double heightStepM);

    /// Least total climb on a grid of `heightStepM`; when the drone's rates leave no way through,
    /// the same without the rates, marked as not within them.
    [[nodiscard]] ClimbProfile energyOptimal() const;
    /// Climb in the take-off zone to the highest floor on the route, then fly level.
    [[nodiscard]] ClimbProfile overLaunch() const;
    /// Climb steadily from the pilot to the highest floor and, on a return, descend steadily to
    /// the pilot: the gentlest straight climb and descent that keep every sample over the floor.
    [[nodiscard]] ClimbProfile gradual() const;

    [[nodiscard]] ProfileStats stats(const std::vector<double>& mslM) const;
    /// The grid step used: the one asked for, coarsened to keep at most 400 heights per sample.
    [[nodiscard]] double heightStepM() const { return step_; }
    /// Whether the drone can fly these heights: no stretch climbs or descends more than `slackMps`
    /// faster than its rates allow.
    [[nodiscard]] bool withinRates(const std::vector<double>& mslM, double slackMps = 0.0) const;

private:
    [[nodiscard]] std::optional<std::vector<double>> search(bool rates) const;
    [[nodiscard]] double highestFloor() const;

    const std::vector<CorridorPoint>& pts_;
    std::vector<Segment> seg_;
    bool returns_;
    double step_;
    double start_;
    double end_;
};

}  // namespace fpv::altitude
