#pragma once

#include "common.hpp"

#include <rfcore/geodesy.hpp>

#include <cstddef>

namespace rfmap {

/// What a route calculation will cost, worked out from the mission before anything runs.
///
/// RouteEvaluator samples every leg at the DEM's step, and at each position with a link it
/// builds and evaluates the terrain profile from the pilot to the drone. The work therefore
/// grows with the positions times the samples of each profile: a leg a few hundred kilometres
/// long, or thousands of waypoints, would hold a request thread for hours. The estimate is an
/// upper bound, so a route it lets through cannot cost more.
class RouteWorkload {
public:
    /// `stepM` is the DEM's native resolution; `withLinks` whether any link is evaluated (with
    /// none, each position only reads the ground height).
    RouteWorkload(const json& mission, double stepM, bool withLinks);

    /// Sampled positions along all legs.
    [[nodiscard]] double positions() const { return positions_; }
    /// Terrain samples the whole calculation reads, at most.
    [[nodiscard]] double samples() const { return samples_; }

    /// HttpError 422 that says how far over `maxSamples` the route is and what to change.
    void require(double maxSamples) const;

private:
    /// Adds the leg from `a` to `b`; `pilot` is where every profile starts.
    void addLeg(const rfcore::GeoPoint& pilot, const rfcore::GeoPoint& a, const rfcore::GeoPoint& b);
    [[nodiscard]] static rfcore::GeoPoint pointOf(const json& p);

    [[no_unique_address]] rfcore::Geodesy geodesy_;
    double stepM_;
    bool withLinks_;
    double positions_ = 0.0;
    double samples_ = 0.0;
};

}  // namespace rfmap
