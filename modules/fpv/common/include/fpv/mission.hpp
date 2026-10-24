#pragma once

#include "fpv/altitude.hpp"
#include "fpv/world.hpp"

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace fpv {

struct Waypoint {
    LatLon position{};
    /// Height in the mission's altitude reference (altM or aglM). For the pilot it is the antenna
    /// height above the ground, which is the same in both references.
    double heightM{};
    std::string label;
};

/// The parts of mission/v1 the FPV modules read. The server has already validated the document
/// against schemas/mission.schema.json.
struct Mission {
    std::string name;
    AltitudeReference altitudeRef = AltitudeReference::Agl;
    Waypoint pilot;
    std::vector<Waypoint> waypoints;
    bool returnToPilot = true;
    std::string evaluate;  ///< both | control | video
    std::string droneId;
    std::string batteryId;
    json raw;

    static Mission fromJson(const json& m) {
        auto wp = [](const json& p, std::string_view field, const std::string& fallback) {
            return Waypoint{{p.at("lat").get<double>(), p.at("lon").get<double>()}, p.at(field).get<double>(),
                            p.value("label", fallback)};
        };
        Mission out;
        out.raw = m;
        out.name = m.value("name", "");
        out.altitudeRef = AltitudeModel::parseReference(m);
        out.pilot = wp(m.at("pilot"), "aglM", "Pilot");
        const std::string_view field = AltitudeModel::waypointField(out.altitudeRef);
        int i = 1;
        for (const json& p : m.at("waypoints")) out.waypoints.push_back(wp(p, field, std::format("WP{}", i++)));
        out.returnToPilot = m.value("returnToPilot", true);
        out.evaluate = m.value("evaluate", "both");
        out.droneId = m.value("drone", "");
        out.batteryId = m.value("battery", "");
        return out;
    }

    /// Links this mission evaluates, in a fixed order.
    [[nodiscard]] std::vector<std::string> links() const {
        if (evaluate == "control") return {"control"};
        if (evaluate == "video") return {"video"};
        return {"control", "video"};
    }
};

}  // namespace fpv
