#pragma once

#include "rfcore/units.hpp"

#include <vector>

namespace rfcore {

/// A point on the WGS84 ellipsoid. Latitude and longitude in degrees.
struct GeoPoint {
    double latitudeDeg{0.0};
    double longitudeDeg{0.0};
};

struct GeodesicInverse {
    Metres distance;          ///< Geodesic distance on the ellipsoid.
    double azimuthStartDeg;   ///< Forward azimuth at the start point, clockwise from north.
    double azimuthEndDeg;     ///< Forward azimuth at the end point.
};

/// Geodesics on WGS84 using Karney's algorithm (GeographicLib), accurate to about
/// 15 nanometres. No spherical approximation is used anywhere in the core.
class Geodesy {
public:
    [[nodiscard]] GeodesicInverse inverse(const GeoPoint& from, const GeoPoint& to) const;
    [[nodiscard]] GeoPoint direct(const GeoPoint& from, double azimuthDeg, Metres distance) const;

    /// Points along the geodesic from `from` to `to`, both ends included, spaced no more
    /// than `maxStep` apart (equal spacing).
    [[nodiscard]] std::vector<GeoPoint> sample(const GeoPoint& from, const GeoPoint& to, Metres maxStep) const;
};

}  // namespace rfcore
