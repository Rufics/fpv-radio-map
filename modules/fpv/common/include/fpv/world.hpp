#pragma once

// The view of the world an FPV module computes on: geodesy, terrain heights and profiles.
// Inside the server it is answered by host calls (HostWorld); in tests and self-tests by
// rf-core directly over a synthetic terrain (LocalWorld). The maths in each module only sees
// this interface, so the same code is tested and shipped.

#include <rfcore/geodesy.hpp>
#include <rfmod/module.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fpv {

using rfmod::json;

struct LatLon {
    double lat;
    double lon;
};

struct InverseResult {
    double distanceM;
    double azimuth1Deg;  ///< forward azimuth at the start
    double azimuth2Deg;  ///< forward azimuth at the end
};

struct DirectResult {
    LatLon point;
    double azimuthDeg;  ///< forward azimuth at the reached point
};

struct GroundSample {
    double distanceM;
    double groundMslM;
};

/// Thrown when terrain data is missing; modules report it instead of guessing.
class MissingTerrain : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IWorld {
public:
    virtual ~IWorld() = default;
    [[nodiscard]] virtual InverseResult inverse(const LatLon& a, const LatLon& b) const = 0;
    [[nodiscard]] virtual DirectResult direct(const LatLon& a, double azimuthDeg, double distanceM) const = 0;
    /// Throws MissingTerrain where the DEM has no data.
    [[nodiscard]] virtual double groundMsl(const LatLon& p) const = 0;
    /// Ground along the geodesic a→b at the DEM's native step, both ends included.
    /// Throws MissingTerrain if any part has no data.
    [[nodiscard]] virtual std::vector<GroundSample> groundProfile(const LatLon& a, const LatLon& b) const = 0;
};

/// The real thing: host calls into the rfmap server.
class HostWorld final : public IWorld {
public:
    explicit HostWorld(const rfmod::HostClient& host) : host_(host) {}

    InverseResult inverse(const LatLon& a, const LatLon& b) const override {
        const json r = host_.call("geodesy.inverse", {{"from", point(a)}, {"to", point(b)}});
        return {r.at("distanceM").get<double>(), r.at("azimuth1Deg").get<double>(), r.at("azimuth2Deg").get<double>()};
    }

    DirectResult direct(const LatLon& a, double azimuthDeg, double distanceM) const override {
        const json r = host_.call("geodesy.direct", {{"from", point(a)}, {"azimuthDeg", azimuthDeg}, {"distanceM", distanceM}});
        return {{r.at("lat").get<double>(), r.at("lon").get<double>()}, r.at("azimuthDeg").get<double>()};
    }

    double groundMsl(const LatLon& p) const override {
        const json r = host_.call("elevation.point", point(p));
        if (r.at("groundMslM").is_null()) throw MissingTerrain("no terrain data at " + describe(p));
        return r.at("groundMslM").get<double>();
    }

    std::vector<GroundSample> groundProfile(const LatLon& a, const LatLon& b) const override {
        const json r = host_.call("profile.get", {{"from", point(a)}, {"to", point(b)}});
        if (!r.value("missing", json::array()).empty()) {
            throw MissingTerrain("no terrain data on part of the path from " + describe(a) + " to " + describe(b));
        }
        std::vector<GroundSample> out;
        for (const json& s : r.at("samples")) out.push_back({s.at("distanceM").get<double>(), s.at("groundMslM").get<double>()});
        return out;
    }

    static json point(const LatLon& p) { return {{"lat", p.lat}, {"lon", p.lon}}; }
    static std::string describe(const LatLon& p) { return std::to_string(p.lat) + ", " + std::to_string(p.lon); }

private:
    const rfmod::HostClient& host_;
};

/// rf-core geodesy over a terrain function: for tests and self-tests.
class LocalWorld final : public IWorld {
public:
    using Terrain = std::function<std::optional<double>(const LatLon&)>;
    LocalWorld(Terrain terrain, double stepM) : terrain_(std::move(terrain)), step_(stepM) {}

    InverseResult inverse(const LatLon& a, const LatLon& b) const override {
        const auto r = geodesy_.inverse({a.lat, a.lon}, {b.lat, b.lon});
        return {r.distance.value(), r.azimuthStartDeg, r.azimuthEndDeg};
    }

    DirectResult direct(const LatLon& a, double azimuthDeg, double distanceM) const override {
        const auto r = geodesy_.directWithAzimuth({a.lat, a.lon}, azimuthDeg, rfcore::Metres(distanceM));
        return {{r.point.latitudeDeg, r.point.longitudeDeg}, r.azimuthEndDeg};
    }

    double groundMsl(const LatLon& p) const override {
        const auto h = terrain_(p);
        if (!h) throw MissingTerrain("no terrain data");
        return *h;
    }

    std::vector<GroundSample> groundProfile(const LatLon& a, const LatLon& b) const override {
        std::vector<GroundSample> out;
        const auto pts = geodesy_.sample({a.lat, a.lon}, {b.lat, b.lon}, rfcore::Metres(step_));
        const double total = inverse(a, b).distanceM;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const LatLon p{pts[i].latitudeDeg, pts[i].longitudeDeg};
            const double d = pts.size() == 1 ? 0.0 : total * static_cast<double>(i) / static_cast<double>(pts.size() - 1);
            out.push_back({d, groundMsl(p)});
        }
        return out;
    }

private:
    Terrain terrain_;
    double step_;
    rfcore::Geodesy geodesy_;
};

}  // namespace fpv
