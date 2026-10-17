#pragma once

#include "rfcore/units.hpp"

namespace rfcore {

/// Earth curvature for radio paths. The radius of the WGS84 ellipsoid in the direction of
/// the path (Euler's formula from the meridian and prime-vertical radii) is scaled by the
/// effective-earth factor k that models atmospheric refraction.
class EarthModel {
public:
    /// @param effectiveEarthFactor k; 4/3 for the standard atmosphere, or derived from a
    ///        forecast refractivity gradient by rf.weather.
    explicit EarthModel(double effectiveEarthFactor);

    [[nodiscard]] double effectiveEarthFactor() const { return k_; }

    /// Radius of curvature of the ellipsoid at a latitude, along an azimuth.
    [[nodiscard]] static Metres radiusOfCurvature(double latitudeDeg, double azimuthDeg);

    /// k times the radius of curvature: the radius of the earth the radio wave "sees".
    [[nodiscard]] Metres effectiveRadius(double latitudeDeg, double azimuthDeg) const;

    /// k from a refractivity gradient dN/dh in N-units per km: k = 1 / (1 + R * dN/dh * 1e-6 / 1000).
    [[nodiscard]] static double factorFromRefractivityGradient(double dNdhPerKm, Metres earthRadius);

private:
    double k_;
};

}  // namespace rfcore
