#include "rfcore/geodesy.hpp"

#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/GeodesicLine.hpp>

#include <cmath>
#include <format>
#include <stdexcept>

namespace rfcore {

GeodesicInverse Geodesy::inverse(const GeoPoint& from, const GeoPoint& to) const {
    const auto& geod = GeographicLib::Geodesic::WGS84();
    double s12 = 0.0;
    double azi1 = 0.0;
    double azi2 = 0.0;
    geod.Inverse(from.latitudeDeg, from.longitudeDeg, to.latitudeDeg, to.longitudeDeg, s12, azi1, azi2);
    return {Metres(s12), azi1, azi2};
}

GeoPoint Geodesy::direct(const GeoPoint& from, double azimuthDeg, Metres distance) const {
    const auto& geod = GeographicLib::Geodesic::WGS84();
    GeoPoint out;
    geod.Direct(from.latitudeDeg, from.longitudeDeg, azimuthDeg, distance.value(), out.latitudeDeg,
                out.longitudeDeg);
    return out;
}

GeodesicDirect Geodesy::directWithAzimuth(const GeoPoint& from, double azimuthDeg, Metres distance) const {
    const auto& geod = GeographicLib::Geodesic::WGS84();
    GeodesicDirect out{};
    geod.Direct(from.latitudeDeg, from.longitudeDeg, azimuthDeg, distance.value(), out.point.latitudeDeg,
                out.point.longitudeDeg, out.azimuthEndDeg);
    return out;
}

std::vector<GeoPoint> Geodesy::sample(const GeoPoint& from, const GeoPoint& to, Metres maxStep) const {
    if (!(maxStep.value() > 0.0)) {
        throw std::invalid_argument("Geodesy::sample: maxStep must be positive");
    }
    const auto& geod = GeographicLib::Geodesic::WGS84();
    const GeographicLib::GeodesicLine line = geod.InverseLine(
        from.latitudeDeg, from.longitudeDeg, to.latitudeDeg, to.longitudeDeg);
    const double total = line.Distance();
    const double wanted = std::max(1.0, std::ceil(total / maxStep.value()));
    if (!(wanted <= static_cast<double>(kMaxIntervals))) {
        throw std::invalid_argument(
            std::format("Geodesy::sample: more than {} intervals; use a larger step", kMaxIntervals));
    }
    const auto intervals = static_cast<std::size_t>(wanted);

    std::vector<GeoPoint> points;
    points.reserve(intervals + 1);
    for (std::size_t i = 0; i <= intervals; ++i) {
        // Distance computed from the index each time, so no error accumulates along the path.
        const double s = total * static_cast<double>(i) / static_cast<double>(intervals);
        GeoPoint p;
        line.Position(s, p.latitudeDeg, p.longitudeDeg);
        points.push_back(p);
    }
    points.front() = from;
    points.back() = to;
    return points;
}

}  // namespace rfcore
