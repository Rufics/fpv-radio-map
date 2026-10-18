#pragma once

#include "fpv/world.hpp"

#include <string>
#include <vector>

namespace fpv {

struct Waypoint {
    LatLon position{};
    double aglM{};
    std::string label;
};

/// The parts of mission/v1 the FPV modules read. The server has already validated the document
/// against schemas/mission.schema.json.
struct Mission {
    std::string name;
    Waypoint pilot;
    std::vector<Waypoint> waypoints;
    bool returnToPilot = true;
    std::string evaluate;  ///< both | control | video
    std::string droneId;
    std::string batteryId;
    json raw;

    static Mission fromJson(const json& m) {
        auto wp = [](const json& p, const std::string& fallback) {
            return Waypoint{{p.at("lat").get<double>(), p.at("lon").get<double>()}, p.at("aglM").get<double>(),
                            p.value("label", fallback)};
        };
        Mission out;
        out.raw = m;
        out.name = m.value("name", "");
        out.pilot = wp(m.at("pilot"), "Pilot");
        int i = 1;
        for (const json& p : m.at("waypoints")) out.waypoints.push_back(wp(p, "WP" + std::to_string(i++)));
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
