#pragma once

// A synthetic but realistic path for the benchmarks: rolling ground at a 30 m step, the
// Copernicus GLO-30 spacing, with a ridge in the middle so the ray is obstructed.

#include "rfcore/terrain_profile.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace rfperf {

inline constexpr double kStepMetres = 30.0;

inline rfcore::TerrainProfile rollingProfile(std::size_t samples) {
    std::vector<rfcore::ProfileSample> points;
    points.reserve(samples);
    const double length = kStepMetres * static_cast<double>(samples - 1);
    for (std::size_t i = 0; i < samples; ++i) {
        const double d = kStepMetres * static_cast<double>(i);
        const double ridge = 60.0 * std::exp(-std::pow((d - length / 2.0) / (length / 10.0), 2.0));
        const double ground = 110.0 + 15.0 * std::sin(d / 700.0) + 6.0 * std::sin(d / 130.0) + ridge;
        points.push_back({rfcore::Metres(d), rfcore::Metres(ground), rfcore::Metres(0.0), rfcore::Metres(4.0),
                          rfcore::GeoPoint{52.23, 21.01}});
    }
    return rfcore::TerrainProfile(std::move(points), "perf synthetic", 80.0, 52.23);
}

}  // namespace rfperf
