#include "route_altitude.hpp"

namespace rfmap {

RouteAltitude::RouteAltitude(const json& mission, const rfcore::IElevationSource& dem)
    : dem_(dem), launch_(mission.value("altitudeRef", "agl") == "launch") {
    if (!launch_) return;
    const json& pilot = mission.at("pilot");
    if (const auto g = dem_.elevation({pilot.at("lat").get<double>(), pilot.at("lon").get<double>()})) {
        launchGround_ = g->heightMsl.value();
    }
}

double RouteAltitude::heightOf(const json& point, bool isPilot) const {
    return point.at(isPilot || !launch_ ? "aglM" : "altM").get<double>();
}

std::optional<double> RouteAltitude::aglAt(const rfcore::GeoPoint& position, double heightM) const {
    if (!launch_) return heightM;
    if (!launchGround_.has_value()) return std::nullopt;
    const auto g = dem_.elevation(position);
    if (!g.has_value()) return std::nullopt;
    return *launchGround_ + heightM - g->heightMsl.value();
}

}  // namespace rfmap
