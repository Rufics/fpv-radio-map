#pragma once

#include <analysis.hpp>  // fpv.battery: energy model
#include <fpv/airframe.hpp>

#include <optional>
#include <string>

namespace fpv::failsafe {

/// Control link margin for the drone at a position. Host: link.evaluate with the mission.
class ILinkProbe {
public:
    virtual ~ILinkProbe() = default;
    [[nodiscard]] virtual double marginDb(const LatLon& p, double aglM) const = 0;
};

class HostLinkProbe final : public ILinkProbe {
public:
    HostLinkProbe(const rfmod::HostClient& host, json mission) : host_(host), mission_(std::move(mission)) {}
    double marginDb(const LatLon& p, double aglM) const override {
        const json r = host_.call("link.evaluate", {{"mission", mission_}, {"link", "control"},
                                                    {"drone", {{"lat", p.lat}, {"lon", p.lon}, {"aglM", aglM}}}});
        return r.at("marginDb").get<double>();
    }

private:
    const rfmod::HostClient& host_;
    json mission_;
};

/// What the flight controller does when the control link drops (set to match the drone).
struct Settings {
    std::string action = "rth";         ///< rth | land | hover | loiter (a fixed-wing loiters for hover)
    std::string altitudeMode = "max";   ///< fixed | max | current: RTH altitude rule
    double rthAltitudeAboveHomeM = 0.0; ///< relative to the pilot's ground, as flight controllers measure it
    double terrainClearanceM = 10.0;    ///< required gap between the drone and terrain plus clutter on the way home
    double stepM = 50.0;
    /// On an autonomous mission the autopilot keeps flying the route when control drops (the
    /// flight controller set to continue the mission on link loss); false runs the failsafe.
    bool continueMission = true;

    static Settings fromJson(const json& options);
};

struct LossPoint {
    double distanceAlongRouteM;
    RoutePoint point;
};

struct RthPlan {
    double cruiseMslM{};             ///< constant altitude flown home
    double homeDistanceM{};
    std::optional<double> regainAfterM;  ///< flown distance until control is back, if it comes back
    std::optional<double> worstTerrainGapM;
    std::optional<LatLon> collision;     ///< first point closer to terrain than the clearance
    double energyWh{};                   ///< from take-off to landing at home via the loss point
};

class FailsafeAnalyzer {
public:
    FailsafeAnalyzer(const IWorld& world, const ILinkProbe& link, Settings settings)
        : world_(world), link_(link), s_(std::move(settings)) {}

    /// First point along the route where the control margin drops below zero, refined by
    /// bisection to 1 m between the last good and the first lost sample.
    [[nodiscard]] std::optional<LossPoint> firstLoss(const Route& route) const { return firstLoss(route, route.outboundLength()); }

    /// The same up to `untilM` along the route: an autonomous mission flies its return leg on
    /// the route too, so control can first drop there.
    [[nodiscard]] std::optional<LossPoint> firstLoss(const Route& route, double untilM) const;

    /// Where control comes back along the rest of the route after `fromM`, sampled every
    /// stepM, if it comes back before the end.
    [[nodiscard]] std::optional<double> regainAlongRoute(const Route& route, double fromM) const;

    /// Return-to-home from the loss point at constant altitude.
    [[nodiscard]] RthPlan planRth(const Mission& mission, const Route& route, const LossPoint& loss,
                                  const battery::EnergyModel* energy) const;

private:
    const IWorld& world_;
    const ILinkProbe& link_;
    Settings s_;
};

json analyze(const json& request, const IWorld& world, const ILinkProbe& link,
             const std::optional<json>& drone, const std::optional<json>& batteryItem);

}  // namespace fpv::failsafe
