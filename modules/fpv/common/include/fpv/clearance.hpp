#pragma once

#include <fpv/route.hpp>
#include <fpv/world.hpp>
#include <rfcore/diffraction.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/path_geometry.hpp>
#include <rfcore/propagation.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fpv {

/// Shared by fpv.min-altitude and fpv.altitude.
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

inline rfcore::TerrainProfile ClearanceSolver::toProfile(const std::vector<GroundSample>& g, double azimuthDeg,
                                                  double latitudeDeg) {
    std::vector<rfcore::ProfileSample> s;
    s.reserve(g.size());
    for (const GroundSample& x : g) {
        s.emplace_back(rfcore::Metres(x.distanceM), rfcore::Metres(x.groundMslM), rfcore::Metres(x.obstacleM),
                       rfcore::Metres(0.0), rfcore::GeoPoint{x.position.lat, x.position.lon});
    }
    return rfcore::TerrainProfile(std::move(s), "route", azimuthDeg, latitudeDeg);
}

inline double ClearanceSolver::clearance(const rfcore::TerrainProfile& profile, double pilotAgl, double droneAgl) const {
    const rfcore::PathGeometry geometry(profile, earth_, rfcore::Metres(pilotAgl), rfcore::Metres(droneAgl));
    const rfcore::BullingtonDiffraction diffraction;
    return diffraction.compute(geometry, lambda_).minFresnelClearance;
}

inline std::optional<double> ClearanceSolver::requiredAgl(const rfcore::TerrainProfile& profile, double pilotAgl,
                                                   double maxAgl) const {
    if (profile.samples().size() < 3) return 0.0;  // no interior point can obstruct
    double prev = 0.0;
    if (clearance(profile, pilotAgl, 0.0) >= fraction_) return 0.0;
    for (std::size_t i = 1; static_cast<double>(i) * step_ <= maxAgl + 1e-9; ++i) {
        const double h = static_cast<double>(i) * step_;
        if (clearance(profile, pilotAgl, h) >= fraction_) {
            double lo = prev;
            double hi = h;
            while (hi - lo > 1e-3) {
                const double mid = (lo + hi) / 2.0;
                (clearance(profile, pilotAgl, mid) >= fraction_ ? hi : lo) = mid;
            }
            return hi;
        }
        prev = h;
    }
    return std::nullopt;
}

/// Effective earth factor and how it was chosen: the forecast's when present, else 4/3.
inline std::pair<double, std::string> refraction(const json& weather) {
    if (weather.is_object() && weather.contains("effectiveEarthFactor") && weather.at("effectiveEarthFactor").is_number()) {
        const double k = weather.at("effectiveEarthFactor").get<double>();
        return {k, "k = " + fixed(k, 3) + " from the forecast refractivity gradient"};
    }
    return {4.0 / 3.0, "standard atmosphere, k = 4/3 (no forecast refraction for this time)"};
}

}  // namespace fpv
