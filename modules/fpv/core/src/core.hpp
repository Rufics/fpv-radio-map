#pragma once

// fpv.core: two scenarios side by side along the route, and KML/GPX export of the route.

#include "fpv/search.hpp"

#include <functional>
#include <memory>
#include <string>

namespace fpv::core {

/// Altitudes written to an export. relativeToGround keeps the AGL the pilot entered and lets the
/// viewer add its own terrain; absolute writes our DEM's ground height plus AGL.
enum class AltitudeMode { RelativeToGround, Absolute };

struct ExportSettings {
    double stepM = 50.0;  ///< vertices along each geodesic leg, so viewers' straight segments stay close
    AltitudeMode altitude = AltitudeMode::RelativeToGround;
};

/// Route vertices: every leg end, plus points every stepM along each leg (exact geodesic points,
/// AGL interpolated as the server does).
std::vector<RoutePoint> exportVertices(const Route& route, double stepM);

std::string toKml(const Mission& mission, const Route& route, const IWorld& world, const ExportSettings& s);
std::string toGpx(const Mission& mission, const Route& route, const IWorld& world, const ExportSettings& s);

/// Escapes the five XML special characters.
std::string xmlEscape(const std::string& text);

/// route.analyze: options.compare {name?, patch} runs a second scenario (mission with the JSON
/// merge patch applied, RFC 7396); options.export lists "kml" and/or "gpx".
/// `evalFor` gives the link evaluator for a mission document.
json analyze(const json& request, const IWorld& world,
             const std::function<std::unique_ptr<ILinkEvaluator>(const json& mission)>& evalFor);

}  // namespace fpv::core
