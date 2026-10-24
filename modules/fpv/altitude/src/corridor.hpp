#pragma once

#include <fpv/clearance.hpp>
#include <fpv/route.hpp>

#include <functional>
#include <map>
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

/// Distances along the route to sample: every `stepM`, and every leg's start and end.
std::vector<double> sampleDistances(const Route& route, double stepM);

/// route.analyze: the corridor along the route (data.corridor), the waypoints in both references,
/// and a warning per leg where the plan leaves the corridor.
json analyze(const json& request, const IWorld& world, const LinkInputs& links);

}  // namespace fpv::altitude
