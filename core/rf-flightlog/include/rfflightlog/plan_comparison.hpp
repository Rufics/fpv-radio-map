#pragma once

// Plan against fact: the mission route and what the planner predicted for it (leg lengths, the
// weakest control-link margin on each leg, the energy needed), next to what the flight log
// shows was flown (distance, energy, how weak the link got, how far the craft strayed).

#include "rfflightlog/story.hpp"
#include "rfflightlog/timeline.hpp"

#include <GeographicLib/LocalCartesian.hpp>
#include <rfcore/geodesy.hpp>

#include <optional>
#include <string>
#include <vector>

namespace rfflightlog {

struct PlannedLeg {
    std::string from;
    std::string to;
    double lengthM = 0.0;
    std::optional<double> controlMarginDb;  ///< the weakest point the planner found on the leg
    std::optional<double> videoMarginDb;
};

class RoutePlan {
public:
    /// The mission (mission/v1) and, when it was evaluated, its last route result.
    static RoutePlan fromMission(const json& mission, const std::optional<json>& routeResult);

    std::vector<rfcore::GeoPoint> points;  ///< pilot, waypoints, and the pilot again on return
    std::vector<PlannedLeg> legs;
    std::optional<double> neededWh;
    [[nodiscard]] double lengthM() const;

private:
    void readResult(const json& result);
};

/// The route flattened onto a local east-north plane around the pilot, to measure how far a
/// position is from it and which leg it is closest to.
class RouteGeometry {
public:
    explicit RouteGeometry(const std::vector<rfcore::GeoPoint>& points);
    struct Nearest {
        std::size_t leg = 0;
        double distanceM = 0.0;
    };
    [[nodiscard]] std::optional<Nearest> nearest(double lat, double lon) const;

private:
    struct Xy {
        double x = 0.0;
        double y = 0.0;
    };
    [[nodiscard]] Xy project(double lat, double lon) const;
    GeographicLib::LocalCartesian frame_;
    std::vector<Xy> points_;
};

class PlanComparison {
public:
    struct Options {
        double offRouteWarnM = 50.0;
    };
    explicit PlanComparison(Options options) : options_(options) {}
    [[nodiscard]] json compare(const RoutePlan& plan, const Timeline& timeline, const FlightSummary& summary) const;

private:
    struct LegFact {
        double seconds = 0.0;
        std::optional<double> minLinkPct;
        double maxOffRouteM = 0.0;
    };
    struct Track {
        std::vector<double> offRouteM;
        std::vector<LegFact> legs;
        std::optional<std::size_t> worst;  ///< sample furthest from the route
        std::size_t worstLeg = 0;
    };
    [[nodiscard]] Track follow(const RoutePlan& plan, const Timeline& timeline) const;
    [[nodiscard]] json rows(const RoutePlan& plan, const FlightSummary& summary, const Track& track) const;
    [[nodiscard]] json findings(const RoutePlan& plan, const FlightSummary& summary, const Track& track,
                                const Timeline& timeline) const;
    static json legsJson(const RoutePlan& plan, const Track& track);
    Options options_;
};

}  // namespace rfflightlog
