#pragma once

#include "corridor.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fpv::altitude {

/// The height route.prepare gives one waypoint, in the mission's altitude reference.
struct AutoHeight {
    std::size_t waypoint;  ///< index into mission.waypoints
    double heightM;        ///< altM or aglM, whole metres, rounded up
    std::string limitedBy; ///< "terrain", a link kind ("control", "video"), or "ceiling"
    double atM;            ///< distance along the route of the sample that set it
    bool blocked;          ///< even the ceiling does not keep the plan inside the corridor
};

/// Heights for the waypoints that leave theirs to the server (Waypoint::autoHeight). Between two
/// points the height changes linearly in the mission's reference, so a waypoint's height is the
/// highest corridor floor on its two legs (terrain and obstacles plus the clearance, and with
/// linkFloor the Fresnel clearance of every link), so a leg between two automatic points flies
/// level over its highest floor, capped by the ceiling. Next to a manual point the line from that
/// point has to clear the floor instead. Manual heights stay as they are. The legs from and back to
/// the pilot are taken as flown level at the waypoint's height, so their highest floor applies;
/// only the climb from the pilot itself is left to the climb-out (the analysis still shows where
/// it runs under the corridor).
class AutoHeightPlanner {
public:
    AutoHeightPlanner(const Mission& mission, const IWorld& world, const Settings& set, const LinkInputs& links, double k);

    [[nodiscard]] std::vector<AutoHeight> plan() const;

private:
    /// One corridor sample in the mission's reference, with the route point it belongs to.
    struct Sample {
        std::size_t leg;
        double t;        ///< fraction along the leg
        double s;
        double floor;    ///< floor in the mission's reference
        double ceiling;
        std::string by;  ///< what sets the floor: "terrain" or a link kind
        bool noLink;     ///< some link has no clear height here up to maxAglM
    };
    /// Which route point (0 = pilot, 1.. = waypoints, the last return = pilot) and whether it is free.
    struct Point {
        double height;
        bool free;
        bool pilot;
        std::string by = "terrain";
        double atM = 0.0;
    };

    [[nodiscard]] std::vector<Sample> samples() const;
    /// The least height a free point needs for one sample `toward` of the way to it from `other`.
    static std::optional<double> need(const Point& other, double toward, double floor);

    const Mission& mission_;
    const IWorld& world_;
    Settings set_;
    const LinkInputs& links_;
    double k_;
};

/// route.prepare: heights for the waypoints without one (or with heightSource "auto"), what set
/// each, and a reason for each that even the ceiling cannot make safe.
json prepare(const json& request, const IWorld& world, const LinkInputs& links);

}  // namespace fpv::altitude
