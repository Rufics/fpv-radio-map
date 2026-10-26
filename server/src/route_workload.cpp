#include "route_workload.hpp"

#include <cmath>
#include <format>
#include <vector>

namespace rfmap {

RouteWorkload::RouteWorkload(const json& mission, double stepM, bool withLinks) : stepM_(stepM), withLinks_(withLinks) {
    // Legs as RouteEvaluator flies them: pilot -> wp1 -> ... -> last, then back when returnToPilot.
    const rfcore::GeoPoint pilot = pointOf(mission.at("pilot"));
    std::vector<rfcore::GeoPoint> points{pilot};
    for (const json& w : mission.at("waypoints")) points.push_back(pointOf(w));
    if (mission.value("returnToPilot", true)) points.push_back(pilot);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) addLeg(pilot, points[i], points[i + 1]);
}

rfcore::GeoPoint RouteWorkload::pointOf(const json& p) { return {p.at("lat").get<double>(), p.at("lon").get<double>()}; }

void RouteWorkload::addLeg(const rfcore::GeoPoint& pilot, const rfcore::GeoPoint& a, const rfcore::GeoPoint& b) {
    const double length = geodesy_.inverse(a, b).distance.value();
    const double positions = std::ceil(length / stepM_) + 1.0;
    positions_ += positions;
    if (!withLinks_) {
        samples_ += positions;
        return;
    }
    // No point of the leg is farther from the pilot than half of (pilot->a + leg + b->pilot):
    // the triangle inequality, from whichever end is closer along the leg.
    const double farthest =
        (geodesy_.inverse(pilot, a).distance.value() + length + geodesy_.inverse(pilot, b).distance.value()) / 2.0;
    samples_ += positions * (std::ceil(farthest / stepM_) + 1.0);
}

void RouteWorkload::require(double maxSamples) const {
    if (samples_ <= maxSamples) return;
    throw HttpError(422,
                    std::format("the route is too large to calculate at once: up to {:.0f} terrain samples over {:.0f} "
                                "positions, and this server allows {:.0f}. Use fewer or shorter legs, or keep the "
                                "waypoints closer to the pilot.",
                                samples_, positions_, maxSamples),
                    {{"samples", samples_}, {"positions", positions_}, {"maxSamples", maxSamples}});
}

}  // namespace rfmap
