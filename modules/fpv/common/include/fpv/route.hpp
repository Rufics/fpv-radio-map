#pragma once

#include "fpv/mission.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

namespace fpv {

/// Thrown when a point is asked of a route that has no legs (a programming error).
class EmptyRouteError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

/// One geodesic leg between two waypoints. The drone's height changes linearly in along-leg
/// distance between the two ends, in the mission's altitude reference (the server's convention).
struct Leg {
    std::size_t index;
    Waypoint from;
    Waypoint to;
    double lengthM;
    double azimuthDeg;   ///< forward azimuth at `from`
    double startM;       ///< distance along the whole route where the leg starts
    bool isReturn;       ///< the last leg back to the pilot
};

struct RoutePoint {
    LatLon position;
    double aglM;         ///< above the ground under the drone; negative when the plan is below it
    std::size_t leg;
    double courseDeg;    ///< forward azimuth of the geodesic at this point
    double heightM;      ///< in the mission's altitude reference
};

/// The route pilot → waypoints → (pilot), measured on the ellipsoid.
class Route {
public:
    /// Throws MissingTerrain when the mission is launch-referenced and the DEM has no ground at the pilot.
    Route(const Mission& mission, const IWorld& world) : world_(world), altitude_(modelFor(mission, world)) {
        std::vector<Waypoint> pts;
        pts.push_back(mission.pilot);
        pts.insert(pts.end(), mission.waypoints.begin(), mission.waypoints.end());
        if (mission.returnToPilot) pts.push_back(mission.pilot);
        double at = 0.0;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const InverseResult inv = world.inverse(pts[i].position, pts[i + 1].position);
            const bool ret = mission.returnToPilot && i + 2 == pts.size();
            legs_.emplace_back(i, pts[i], pts[i + 1], inv.distanceM, inv.azimuth1Deg, at, ret);
            at += inv.distanceM;
        }
        length_ = at;
        outbound_ = mission.returnToPilot && !legs_.empty() ? legs_.back().startM : length_;
    }

    [[nodiscard]] const std::vector<Leg>& legs() const { return legs_; }
    [[nodiscard]] const AltitudeModel& altitude() const { return altitude_; }
    [[nodiscard]] double length() const { return length_; }
    /// Length up to the target (the last waypoint), without the return leg.
    [[nodiscard]] double outboundLength() const { return outbound_; }

    /// The exact point at a distance along the route (geodesic direct from the leg start).
    [[nodiscard]] RoutePoint at(double s) const {
        if (legs_.empty()) throw EmptyRouteError("empty route");
        s = std::clamp(s, 0.0, length_);
        std::size_t i = 0;
        while (i + 1 < legs_.size() && s > legs_[i].startM + legs_[i].lengthM) ++i;
        const Leg& leg = legs_[i];
        const double along = std::clamp(s - leg.startM, 0.0, leg.lengthM);
        const DirectResult d = world_.direct(leg.from.position, leg.azimuthDeg, along);
        const double t = leg.lengthM > 0.0 ? along / leg.lengthM : 0.0;
        const double height = leg.from.heightM + t * (leg.to.heightM - leg.from.heightM);
        return {d.point, aglAt(d.point, height), i, d.azimuthDeg, height};
    }

    /// Height above the ground at a position for a height in the mission's reference. Asks the
    /// world for the ground only when the reference is the launch point.
    [[nodiscard]] double aglAt(const LatLon& p, double heightM) const {
        if (altitude_.followsTerrain()) return heightM;
        return altitude_.aglM(heightM, world_.groundMsl(p));
    }

private:
    static AltitudeModel modelFor(const Mission& mission, const IWorld& world) {
        using enum AltitudeReference;
        if (mission.altitudeRef == Agl) return {Agl, 0.0};
        return {Launch, world.groundMsl(mission.pilot.position)};
    }

    const IWorld& world_;
    AltitudeModel altitude_;
    std::vector<Leg> legs_;
    double length_ = 0.0;
    double outbound_ = 0.0;
};

/// A "why" line (OpenAPI Reason).
inline json reason(const std::string& moduleId, const std::string& severity, const std::string& text,
                   const json& fix = nullptr) {
    json r = {{"module", moduleId}, {"severity", severity}, {"text", text}};
    if (!fix.is_null()) r["fix"] = fix;
    return r;
}

/// Formats a number for preview text with a fixed number of decimals. Display only.
inline std::string fixed(double v, int decimals) {
    return std::format("{:.{}f}", v, std::max(decimals, 0));
}

}  // namespace fpv
