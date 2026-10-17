#include "clearance.hpp"

#include <rfcore/path_geometry.hpp>

namespace fpv::minalt {

rfcore::TerrainProfile ClearanceSolver::toProfile(const std::vector<GroundSample>& g, double azimuthDeg,
                                                  double latitudeDeg) {
    std::vector<rfcore::ProfileSample> s;
    s.reserve(g.size());
    for (const GroundSample& x : g) {
        s.push_back({rfcore::Metres(x.distanceM), rfcore::Metres(x.groundMslM), rfcore::Metres(x.obstacleM),
                     rfcore::Metres(0.0), rfcore::GeoPoint{x.position.lat, x.position.lon}});
    }
    return rfcore::TerrainProfile(std::move(s), "route", azimuthDeg, latitudeDeg);
}

double ClearanceSolver::clearance(const rfcore::TerrainProfile& profile, double pilotAgl, double droneAgl) const {
    const rfcore::PathGeometry geometry(profile, earth_, rfcore::Metres(pilotAgl), rfcore::Metres(droneAgl));
    const rfcore::BullingtonDiffraction diffraction;
    return diffraction.compute(geometry, lambda_).minFresnelClearance;
}

std::optional<double> ClearanceSolver::requiredAgl(const rfcore::TerrainProfile& profile, double pilotAgl,
                                                   double maxAgl) const {
    if (profile.samples().size() < 3) return 0.0;  // no interior point can obstruct
    double prev = 0.0;
    if (clearance(profile, pilotAgl, 0.0) >= fraction_) return 0.0;
    for (double h = step_; h <= maxAgl + 1e-9; h += step_) {
        if (clearance(profile, pilotAgl, h) >= fraction_) {
            double lo = prev, hi = h;
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

}  // namespace fpv::minalt
