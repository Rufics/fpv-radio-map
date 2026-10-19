// fpv.battery analysis: energy along the route, point of no return and turn-back suggestion.

#include "analysis.hpp"

#include <cmath>

namespace fpv::battery {
namespace {

constexpr const char* kId = "fpv.battery";

Air airFrom(const json& weather, const json& options) {
    Air a;
    auto num = [](const json& j, const char* k) {
        return j.is_object() && j.contains(k) && j.at(k).is_number() ? std::optional<double>(j.at(k).get<double>()) : std::nullopt;
    };
    a.densityKgM3 = num(weather, "airDensityKgM3");
    a.windSpeedMps = num(weather, "windSpeedMps");
    a.windFromDeg = num(weather, "windFromDeg");
    a.temperatureC = num(weather, "temperatureC");
    if (auto v = num(options, "temperatureC"); v.has_value()) a.temperatureC = v;
    if (auto v = num(options, "windSpeedMps"); v.has_value()) a.windSpeedMps = v;
    if (auto v = num(options, "windFromDeg"); v.has_value()) a.windFromDeg = v;
    return a;
}

json resultJson(const Plan& p) {
    json r = {{"energyWh", p.energyWh}, {"usableWh", p.usableWh}, {"neededWh", p.neededWh},
              {"rangeM", p.rangeM},     {"neededM", p.neededM},   {"shortfallM", p.shortfallM},
              {"notes", p.notes}};
    if (p.turnBack) {
        r["turnBack"] = {{"distanceAlongRouteM", p.turnBack->distanceAlongRouteM},
                         {"returnM", p.turnBack->returnM},
                         {"lat", p.turnBack->position.lat},
                         {"lon", p.turnBack->position.lon},
                         {"aglM", p.turnBack->aglM}};
    }
    return r;
}

}  // namespace

/// The analysis itself, independent of where the world comes from.
json analyze(const json& request, const IWorld& world, const json& drone, const json& battery) {
    const Mission mission = Mission::fromJson(request.at("mission"));
    const json options = request.value("options", json::object());
    const json weather = request.value("weather", json());
    const Air air = airFrom(weather, options);
    json reasons = json::array();
    json data = json::object();
    try {
        const EnergyModel model(DroneSpec::fromJson(drone), air, world);
        const BatterySpec spec = BatterySpec::fromJson(battery);
        const Route route(mission, world);
        const BatteryPlanner planner(model, spec, options.value("scanStepM", 50.0));
        const Plan p = planner.plan(mission, route, world, air.temperatureC);
        data = resultJson(p);
        data["drone"] = {{"id", drone.value("id", "")}, {"version", drone.value("version", 0)}};
        data["battery"] = {{"id", battery.value("id", "")}, {"version", battery.value("version", 0)}};

        // The energy model takes wind without a direction as calm air too.
        if (!air.windSpeedMps.has_value() || !air.windFromDeg.has_value()) {
            reasons.push_back(reason(kId, "warn", "No wind forecast for this time: energy is for calm air."));
        } else {
            data["notes"].push_back("Wind " + fixed(*air.windSpeedMps, 1) + " m/s from " + fixed(*air.windFromDeg, 0) +
                                    "°, taken as steady over the whole route (forecast at 10 m above ground).");
        }
        const double pct = 100.0 * p.neededWh / p.usableWh;
        if (p.neededWh <= p.usableWh) {
            reasons.push_back(reason(kId, pct > 90.0 ? "warn" : "info",
                                     "Energy: the route needs " + fixed(p.neededWh, 1) + " Wh of " + fixed(p.usableWh, 1) +
                                         " Wh usable (" + fixed(pct, 0) + " %)."));
        } else {
            json fix = nullptr;
            if (p.turnBack) {
                // Keep the waypoints before the leg the turn-back point is on, then add T.
                json wps = json::array();
                const auto& orig = request.at("mission").at("waypoints");
                for (std::size_t i = 0; i + 1 <= p.turnBack->leg && i < orig.size(); ++i) wps.push_back(orig[i]);
                wps.push_back({{"lat", p.turnBack->position.lat}, {"lon", p.turnBack->position.lon},
                               {"aglM", p.turnBack->aglM}, {"label", "T"}});
                const double after = p.turnBack->distanceAlongRouteM + p.turnBack->returnM;
                fix = {{"id", "fpv.battery/turn-back"},
                       {"title", "Turn back at T"},
                       {"patch", {{"waypoints", wps}, {"returnToPilot", true}}},
                       {"preview",
                        {{{"label", "Route"}, {"now", fixed(p.neededM / 1000.0, 2) + " km"}, {"after", fixed(after / 1000.0, 2) + " km"}},
                         {{"label", "Energy needed"}, {"now", fixed(p.neededWh, 1) + " Wh"}, {"after", "≤ " + fixed(p.usableWh, 1) + " Wh"}},
                         {{"label", "Last point"},
                          {"now", mission.waypoints.back().label},
                          {"after", "T, " + fixed(p.turnBack->distanceAlongRouteM / 1000.0, 2) + " km out"}}}}};
            }
            reasons.push_back(reason(kId, "blocked",
                                     "Battery: usable energy runs out after " + fixed(p.rangeM / 1000.0, 2) + " km of " +
                                         fixed(p.neededM / 1000.0, 2) + " km (" + fixed(p.neededWh, 1) + " Wh needed, " +
                                         fixed(p.usableWh, 1) + " Wh usable).",
                                     fix));
        }
    } catch (const Unflyable& e) {
        reasons.push_back(reason(kId, "blocked", std::string("Wind: ") + e.what() + "."));
    } catch (const MissingTerrain& e) {
        reasons.push_back(reason(kId, "warn", std::string("Battery check skipped: ") + e.what() + "."));
    } catch (const std::domain_error& e) {
        reasons.push_back(reason(kId, "warn", std::string("Battery check skipped: ") + e.what() + "."));
    }
    return {{"reasons", reasons}, {"data", data}};
}

}  // namespace fpv::battery
