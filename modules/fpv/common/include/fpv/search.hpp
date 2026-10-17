#pragma once

// Shared by fpv.relay and fpv.best-launch: candidate points inside a drawn area, and link
// margins between a ground point and the drone along the route.

#include "fpv/route.hpp"

#include <rfcore/constants.hpp>

#include <cmath>
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
        for (const json& p : ring) a.ring_.push_back({p.at("lat").get<double>(), p.at("lon").get<double>()});
        if (a.ring_.size() < 3) throw rfmod::BadRequest("an area needs at least three points");
        return a;
    }

    [[nodiscard]] bool contains(const LatLon& p) const {
        bool inside = false;
        const double px = p.lon, py = mercY(p.lat);
        for (std::size_t i = 0, j = ring_.size() - 1; i < ring_.size(); j = i++) {
            const double xi = ring_[i].lon, yi = mercY(ring_[i].lat);
            const double xj = ring_[j].lon, yj = mercY(ring_[j].lat);
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
        }
        return inside;
    }

    /// Grid points inside the area, spaced `cellM` along geodesics north and east from the
    /// area's south-west corner. Throws BadRequest above `maxPoints`.
    [[nodiscard]] std::vector<LatLon> grid(const IWorld& world, double cellM, std::size_t maxPoints) const {
        double s = 90, w = 180, n = -90, e = -180;
        for (const LatLon& p : ring_) {
            s = std::min(s, p.lat);
            n = std::max(n, p.lat);
            w = std::min(w, p.lon);
            e = std::max(e, p.lon);
        }
        const LatLon sw{s, w};
        const double height = world.inverse(sw, {n, w}).distanceM;
        std::vector<LatLon> out;
        for (double dy = cellM / 2.0; dy < height; dy += cellM) {
            const LatLon rowStart = world.direct(sw, 0.0, dy).point;
            const double width = world.inverse(rowStart, {rowStart.lat, e}).distanceM;
            for (double dx = cellM / 2.0; dx < width; dx += cellM) {
                const LatLon p = world.direct(rowStart, 90.0, dx).point;
                if (contains(p)) {
                    out.push_back(p);
                    if (out.size() > maxPoints) {
                        throw rfmod::BadRequest("the area holds more than " + std::to_string(maxPoints) +
                                                " candidate points at " + std::to_string(static_cast<int>(cellM)) +
                                                " m; draw a smaller area or use a larger cell");
                    }
                }
            }
        }
        return out;
    }

private:
    static double mercY(double latDeg) { return std::asinh(std::tan(latDeg * rfcore::constants::kPi / 180.0)); }
    std::vector<LatLon> ring_;
};

/// Drone positions checked along the route: every `stepM` on the way out, and the target.
inline std::vector<RoutePoint> routeSamples(const Route& route, double stepM) {
    std::vector<RoutePoint> out;
    const double outLen = route.outboundLength();
    for (double s = stepM; s < outLen; s += stepM) out.push_back(route.at(s));
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
