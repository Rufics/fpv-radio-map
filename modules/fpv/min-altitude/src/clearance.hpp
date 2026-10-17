#pragma once

#include <fpv/world.hpp>
#include <rfcore/diffraction.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/propagation.hpp>

#include <optional>
#include <vector>

namespace fpv::minalt {

/// Lowest drone height above ground at which a fraction of the first Fresnel zone is clear
/// along the whole path from the pilot's antenna, in rf-core's exact effective-earth geometry.
class ClearanceSolver {
public:
    ClearanceSolver(double effectiveEarthFactor, rfcore::Hertz frequency, double fraction, double scanStepM = 0.5)
        : earth_(effectiveEarthFactor), lambda_(rfcore::wavelength(frequency)), fraction_(fraction), step_(scanStepM) {}

    /// Clearance ratio (worst sample's distance below the ray over the first Fresnel radius there)
    /// with the drone at `droneAgl`. ≥ 1 means the whole first zone is clear.
    [[nodiscard]] double clearance(const rfcore::TerrainProfile& profile, double pilotAgl, double droneAgl) const;

    /// Scans upwards from 0 m in `scanStepM` steps to the first height that is clear, then
    /// bisects that step to 1 mm. Nothing if no height up to `maxAgl` is clear.
    [[nodiscard]] std::optional<double> requiredAgl(const rfcore::TerrainProfile& profile, double pilotAgl,
                                                    double maxAgl) const;

    /// rf-core profile from ground samples (positions and DEM error are not used here).
    static rfcore::TerrainProfile toProfile(const std::vector<GroundSample>& g, double azimuthDeg, double latitudeDeg);

private:
    rfcore::EarthModel earth_;
    rfcore::Metres lambda_;
    double fraction_;
    double step_;
};

}  // namespace fpv::minalt
