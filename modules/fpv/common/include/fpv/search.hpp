#pragma once

// Shared by fpv.relay and fpv.best-launch: candidate points inside a drawn area, and link
// margins between a ground point and the drone along the route.

#include "fpv/route.hpp"

#include <rfcore/constants.hpp>

#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fpv {

struct End {
    LatLon position;
    double aglM;
};

/// Link margin between a ground end and an air end. Host: link.evaluate with the mission shortcut.
class ILinkEvaluator {
public:
    virtual ~ILinkEvaluator() = default;
    [[nodiscard]] virtual double marginDb(const std::string& link, const End& ground, const End& drone) const = 0;
};

class HostLinkEvaluator final : public ILinkEvaluator {
public:
    HostLinkEvaluator(const rfmod::HostClient& host, json mission) : host_(host), mission_(std::move(mission)) {}
    double marginDb(const std::string& link, const End& ground, const End& drone) const override {
        const json r = host_.call("link.evaluate",
                                  {{"mission", mission_},
                                   {"link", link},
                                   {"ground", {{"lat", ground.position.lat}, {"lon", ground.position.lon}, {"aglM", ground.aglM}}},
                                   {"drone", {{"lat", drone.position.lat}, {"lon", drone.position.lon}, {"aglM", drone.aglM}}}});
        return r.at("marginDb").get<double>();
    }

private:
    const rfmod::HostClient& host_;
    json mission_;
};

/// A polygon as drawn on the web map. Map edges are straight in Web Mercator, so the inside
/// test runs in Mercator coordinates (x = lon, y = asinh(tan lat)), which is exact for it.
class DrawnArea {
public:
    static DrawnArea fromJson(const json& ring) {
        DrawnArea a;
        for (const json& p : ring) {
            a.ring_.push_back({p.at("lat").get<double>(), p.at("lon").get<double>()});
            a.mercY_.push_back(mercY(a.ring_.back().lat));
        }
        if (a.ring_.size() < 3) throw rfmod::BadRequest("an area needs at least three points");
        return a;
    }

    [[nodiscard]] bool contains(const LatLon& p) const {
        bool inside = false;
        const double px = p.lon;
        const double py = mercY(p.lat);
        for (std::size_t i = 0, j = ring_.size() - 1; i < ring_.size(); j = i++) {
            const double xi = ring_[i].lon;
            const double yi = mercY_[i];
            const double xj = ring_[j].lon;
            const double yj = mercY_[j];
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
        }
        return inside;
    }

    /// Grid points inside the area, spaced `cellM` along geodesics north and east from the
    /// area's south-west corner. Throws BadRequest above `maxPoints`.
    [[nodiscard]] std::vector<LatLon> grid(const IWorld& world, double cellM, std::size_t maxPoints) const {
        if (!(cellM >= kMinCellM)) throw rfmod::BadRequest(std::format("cellM must be at least {} m", kMinCellM));
        double s = 90;
        double w = 180;
        double n = -90;
        double e = -180;
        for (const LatLon& p : ring_) {
            s = std::min(s, p.lat);
            n = std::max(n, p.lat);
            w = std::min(w, p.lon);
            e = std::max(e, p.lon);
        }
        const LatLon sw{s, w};
        const double height = world.inverse(sw, {n, w}).distanceM;
        // The loops visit every cell of the bounding box, inside the area or not: a thin diagonal
        // area over a wide box costs the box, so the box is bounded as well as the points.
        const double widest = std::max(world.inverse(sw, {s, e}).distanceM, world.inverse({n, w}, {n, e}).distanceM);
        if ((height / cellM) * (widest / cellM) > kMaxGridCells) throw tooManyCells(cellM);
        std::vector<LatLon> out;
        // The estimate above uses the box's edges; rows between them can be wider (a box across
        // the equator is widest in the middle), so the cells visited are counted as well.
        double visited = 0.0;
        for (std::size_t row = 0; (static_cast<double>(row) + 0.5) * cellM < height; ++row) {
            const double dy = (static_cast<double>(row) + 0.5) * cellM;
            const LatLon rowStart = world.direct(sw, 0.0, dy).point;
            const double width = world.inverse(rowStart, {rowStart.lat, e}).distanceM;
            for (std::size_t col = 0; (static_cast<double>(col) + 0.5) * cellM < width; ++col) {
                if (++visited > kMaxGridCells) throw tooManyCells(cellM);
                const double dx = (static_cast<double>(col) + 0.5) * cellM;
                const LatLon p = world.direct(rowStart, 90.0, dx).point;
                if (!contains(p)) continue;
                out.push_back(p);
                if (out.size() > maxPoints) {
                    throw rfmod::BadRequest(std::format("the area holds more than {} candidate points at {} m; draw a "
                                                        "smaller area or use a larger cell",
                                                        maxPoints, static_cast<int>(cellM)));
                }
            }
        }
        return out;
    }

    [[nodiscard]] static rfmod::BadRequest tooManyCells(double cellM) {
        return rfmod::BadRequest(std::format("the area's bounding box holds more than {} cells at {} m; draw a "
                                             "smaller area or use a larger cell",
                                             kMaxGridCells, static_cast<int>(cellM)));
    }

    /// A cell below this would make the grid loops run for minutes before maxPoints stops them.
    static constexpr double kMinCellM = 1.0;
    /// Bounding-box cells a grid may visit: a few seconds of geodesic steps at most.
    static constexpr double kMaxGridCells = 2'000'000.0;

private:
    static double mercY(double latDeg) { return std::asinh(std::tan(latDeg * rfcore::constants::kPi / 180.0)); }
    std::vector<LatLon> ring_;
    std::vector<double> mercY_;  ///< the ring's Mercator y, one per vertex: contains() runs per grid cell
};

/// Samples along a route, at most this many: a step too small for the route is a bad request,
/// not a loop that runs until memory ends.
constexpr std::size_t kMaxRouteSamples = 100000;

/// Drone positions checked along the route: every `stepM` on the way out, and the target.
/// Throws BadRequest for a step that is not positive or gives more than kMaxRouteSamples.
inline std::vector<RoutePoint> routeSamples(const Route& route, double stepM) {
    if (!(stepM > 0.0)) throw rfmod::BadRequest("stepM must be positive");
    const double outLen = route.outboundLength();
    if (outLen / stepM > static_cast<double>(kMaxRouteSamples)) {
        throw rfmod::BadRequest(std::format("stepM {} m gives more than {} samples on this route; use a larger step",
                                            stepM, kMaxRouteSamples));
    }
    std::vector<RoutePoint> out;
    for (std::size_t i = 1; static_cast<double>(i) * stepM < outLen; ++i) out.push_back(route.at(static_cast<double>(i) * stepM));
    out.push_back(route.at(outLen));
    return out;
}

struct WorstMargin {
    double marginDb = std::numeric_limits<double>::infinity();
    std::string link;
    std::size_t sample = 0;
};

/// The worst margin over every sample and every link, with the ground end fixed.
inline WorstMargin worstAlongRoute(const ILinkEvaluator& eval, const std::vector<std::string>& links, const End& ground,
                                   const std::vector<RoutePoint>& samples) {
    WorstMargin w;
    for (const std::string& link : links) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double m = eval.marginDb(link, ground, {samples[i].position, samples[i].aglM});
            if (m < w.marginDb) w = {m, link, i};
        }
    }
    return w;
}

}  // namespace fpv
