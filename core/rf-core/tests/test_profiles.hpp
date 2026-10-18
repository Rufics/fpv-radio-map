#pragma once

#include "rfcore/terrain_profile.hpp"

#include <utility>
#include <vector>

namespace rfcore::test {

/// Profile from (distance, ground MSL) pairs; positions are not used by the maths under test.
inline TerrainProfile makeProfile(const std::vector<std::pair<double, double>>& points, double azimuthDeg = 37.5,
                                  double latitudeDeg = 48.53) {
    std::vector<ProfileSample> samples;
    samples.reserve(points.size());
    for (const auto& [d, h] : points) {
        samples.push_back({Metres(d), Metres(h), Metres(0.0), Metres(4.0), GeoPoint{latitudeDeg, 35.2}});
    }
    return TerrainProfile(std::move(samples), "test fixture", azimuthDeg, latitudeDeg);
}

/// Flat ground at `ground` with one spike of height `top` at distance `at`, length `length`.
inline TerrainProfile spikeProfile(double length, double at, double ground, double top, double step = 30.0) {
    std::vector<std::pair<double, double>> pts;
    for (std::size_t i = 0; static_cast<double>(i) * step < length; ++i) {
        const double d = static_cast<double>(i) * step;
        if (d < at && d + step > at) {
            pts.emplace_back(d, ground);
            pts.emplace_back(at, top);
            continue;
        }
        pts.emplace_back(d, ground);
    }
    pts.emplace_back(length, ground);
    return makeProfile(pts);
}

}  // namespace rfcore::test
