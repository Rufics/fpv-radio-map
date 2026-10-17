#include "rfcore/path_geometry.hpp"

#include <cmath>

namespace rfcore {

PathGeometry::PathGeometry(const TerrainProfile& profile, const EarthModel& earth, Metres antennaAglStart,
                           Metres antennaAglEnd)
    : effectiveRadius_(earth.effectiveRadius(profile.midLatitudeDeg(), profile.azimuthDeg())),
      baseHeight_(profile.samples().front().groundMsl) {
    const auto& s = profile.samples();
    start_ = toPlane(s.front().distance, s.front().groundMsl + antennaAglStart);
    end_ = toPlane(s.back().distance, s.back().groundMsl + antennaAglEnd);
    tops_.reserve(s.size());
    for (const ProfileSample& sample : s) {
        tops_.push_back(toPlane(sample.distance, sample.topMsl()));
    }
}

PlanePoint PathGeometry::toPlane(Metres distanceAlongGround, Metres heightMsl) const {
    // Heights are measured from the base height so that the numbers stay small and the
    // subtraction a + h - a does not lose metres-scale digits to the 6e6 m radius.
    const double a = effectiveRadius_.value();
    const double theta = distanceAlongGround.value() / a;
    const double h = (heightMsl - baseHeight_).value();
    // y = (a + h) cos θ - a, written as h cos θ - a (1 - cos θ) with 1 - cos θ = 2 sin²(θ/2).
    const double halfSin = std::sin(theta / 2.0);
    return {(a + h) * std::sin(theta), h * std::cos(theta) - 2.0 * a * halfSin * halfSin};
}

Metres PathGeometry::rayLength() const {
    return Metres(std::hypot(end_.x - start_.x, end_.y - start_.y));
}

RayRelation PathGeometry::relate(const PlanePoint& p) const {
    const double dx = end_.x - start_.x;
    const double dy = end_.y - start_.y;
    const double len = std::hypot(dx, dy);
    const double px = p.x - start_.x;
    const double py = p.y - start_.y;
    const double along = (px * dx + py * dy) / len;
    const double above = (dx * py - dy * px) / len;
    return {Metres(along), Metres(len - along), Metres(above)};
}

}  // namespace rfcore
