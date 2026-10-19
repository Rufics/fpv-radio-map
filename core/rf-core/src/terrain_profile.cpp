#include "rfcore/terrain_profile.hpp"

#include <stdexcept>

namespace rfcore {

TerrainProfile::TerrainProfile(std::vector<ProfileSample> samples, std::string sourceName, double azimuthDeg,
                               double midLatitudeDeg)
    : samples_(std::move(samples)),
      sourceName_(std::move(sourceName)),
      azimuthDeg_(azimuthDeg),
      midLatitudeDeg_(midLatitudeDeg) {
    if (samples_.size() < 2) {
        throw std::invalid_argument("TerrainProfile: needs at least the two end points");
    }
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        if (!(samples_[i].distance > samples_[i - 1].distance)) {
            throw std::invalid_argument("TerrainProfile: distances must increase strictly");
        }
    }
}

ProfileBuilder::ProfileBuilder(const IElevationSource& source, Geodesy geodesy)
    : source_(source), geodesy_(geodesy) {}

ProfileResult ProfileBuilder::build(const GeoPoint& from, const GeoPoint& to, std::optional<Metres> step) const {
    Metres spacing = source_.nativeResolution();
    if (step && *step < spacing) {
        spacing = *step;
    }
    const GeodesicInverse inv = geodesy_.inverse(from, to);
    if (!(inv.distance.value() > 0.0)) {
        throw std::invalid_argument("ProfileBuilder: start and end are the same point");
    }
    const std::vector<GeoPoint> points = geodesy_.sample(from, to, spacing);
    const auto n = static_cast<double>(points.size() - 1);

    std::vector<ProfileSample> samples;
    samples.reserve(points.size());
    ProfileResult result;
    std::optional<Metres> gapStart;

    for (std::size_t i = 0; i < points.size(); ++i) {
        const Metres distance = inv.distance * (static_cast<double>(i) / n);
        const std::optional<ElevationSample> e = source_.elevation(points[i]);
        if (!e) {
            if (!gapStart) {
                gapStart = distance;
            }
            continue;
        }
        if (gapStart) {
            result.missing.push_back({*gapStart, distance});
            gapStart.reset();
        }
        samples.emplace_back(distance, e->heightMsl, Metres(0.0), e->uncertainty, points[i]);
    }
    if (gapStart) {
        result.missing.push_back({*gapStart, inv.distance});
    }
    if (result.missing.empty()) {
        const GeoPoint mid = geodesy_.direct(from, inv.azimuthStartDeg, inv.distance / 2.0);
        const GeodesicInverse midInv = geodesy_.inverse(mid, to);
        result.profile.emplace(std::move(samples), source_.name(), midInv.azimuthStartDeg, mid.latitudeDeg);
    }
    return result;
}

}  // namespace rfcore
