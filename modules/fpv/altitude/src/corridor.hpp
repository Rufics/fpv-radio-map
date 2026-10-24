#pragma once

#include <fpv/clearance.hpp>
#include <fpv/route.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fpv::altitude {

using fpv::LinkInput;
using fpv::LinkInputs;

/// Request options, with the schema's defaults.
struct Settings {
    double stepM = 100.0;
    double clearanceM = 30.0;
    double takeoffM = 200.0;
    double ceilingAglM = 120.0;
    bool linkFloor = true;
    double fraction = 0.6;
    double maxAglM = 500.0;

    static Settings fromOptions(const json& options);
};

/// The ground along one leg, from the host's terrain profile between its ends.
class LegGround {
public:
    explicit LegGround(std::vector<GroundSample> samples) : g_(std::move(samples)) {}

    /// Ground height `alongM` from the leg's start, interpolated between profile samples.
    [[nodiscard]] double groundAt(double alongM) const;
    /// Highest ground plus obstacles between two distances along the leg, ends included.
    [[nodiscard]] double highestIn(double fromM, double toM) const;

private:
    std::vector<GroundSample> g_;
};

/// One corridor sample: the planned altitude and the limits around it, all above mean sea level.
struct CorridorPoint {
    double s;
    RoutePoint p;
    double groundMslM;
    double plannedMslM;
    double terrainFloorMslM;               ///< highest ground and obstacles near the sample + clearance
    bool nearPilot;                        ///< within the take-off zone, where the clearance ramps up from 0
    std::optional<double> linkFloorMslM;   ///< where the link's Fresnel clearance holds; none if no height does
    bool linkChecked = false;
    double ceilingMslM;
    double fresnelRadiusM;                 ///< first-zone radius half way from the pilot, at the lowest frequency
    std::string linkFloorBy;               ///< the link kind that sets the link floor (or has no clear height)

    [[nodiscard]] double floorMslM() const { return linkFloorMslM.has_value() ? std::max(terrainFloorMslM, *linkFloorMslM) : terrainFloorMslM; }
    /// "terrain", or the link kind when the link floor is the higher one.
    [[nodiscard]] std::string floorBy() const { return linkFloorMslM.has_value() && *linkFloorMslM > terrainFloorMslM ? linkFloorBy : "terrain"; }
};

/// Builds the corridor: the ground along every leg from the host's terrain profile, and at each
/// sample the terrain floor, the ceiling and (with linkFloor) the lowest height where every
/// link's Fresnel clearance holds.
class CorridorBuilder {
public:
    CorridorBuilder(const Mission& mission, const Route& route, const IWorld& world, const Settings& set,
                    const LinkInputs& links, double k);

    /// The corridor at every sample distance (sampleDistances).
    [[nodiscard]] std::vector<CorridorPoint> build() const;
    /// The corridor at one distance along the route.
    [[nodiscard]] CorridorPoint at(double s) const;

private:
    struct LinkSolver {
        std::string kind;
        ClearanceSolver solver;
    };
    /// The link floor at one point: the highest of the links' lowest clear heights, as MSL, and
    /// the link that sets it. Nothing when some link has no clear height up to the search limit.
    struct LinkFloor {
        std::optional<double> mslM;
        std::string by;
    };

    [[nodiscard]] LinkFloor linkFloor(const RoutePoint& p, const InverseResult& inv) const;

    const Mission& mission_;
    const Route& route_;
    const IWorld& world_;
    Settings set_;
    std::vector<LinkSolver> solvers_;
    std::vector<LegGround> ground_;
    double lambda_ = 0.0;
};

/// Distances along the route to sample: every `stepM`, and every leg's start and end.
std::vector<double> sampleDistances(const Route& route, double stepM);

/// route.analyze: the corridor along the route (data.corridor), the waypoints in both references,
/// and a warning per leg where the plan leaves the corridor.
json analyze(const json& request, const IWorld& world, const LinkInputs& links);

}  // namespace fpv::altitude
