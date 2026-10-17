// fpv.battery module: host glue and self-test.

#include "analysis.hpp"

#include <cmath>

namespace fpv::battery {
namespace {
constexpr const char* kId = "fpv.battery";
constexpr const char* kManifest = R"json({"id":"fpv.battery","version":"1.0.0"})json";
}  // namespace

class BatteryModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const Mission m = Mission::fromJson(request.at("mission"));
        if (m.droneId.empty() || m.batteryId.empty()) {
            return {{"reasons", {reason(kId, "info", "Pick a drone and a battery to check the energy for this route.")}},
                    {"data", json::object()}};
        }
        const json drone = host().call("library.get", {{"kind", "drones"}, {"id", m.droneId}});
        const json battery = host().call("library.get", {{"kind", "batteries"}, {"id", m.batteryId}});
        json req = request;
        if (!req.contains("weather") || req.at("weather").is_null()) {
            try {
                req["weather"] = host().call("weather.point", {{"lat", m.pilot.position.lat},
                                                                {"lon", m.pilot.position.lon},
                                                                {"time", request.value("time", "")}});
            } catch (const rfmod::HostError&) {
                req["weather"] = nullptr;
            }
        }
        const HostWorld world(host());
        return analyze(req, world, drone, battery);
    }

    json selftest() override;
};

json BatteryModule::selftest() {
    // Flat ground, one 2 km leg due east and back, 10 m/s airspeed, 100 W.
    const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
    const json drone = {{"id", "t"}, {"massKg", 1.0}, {"cruiseAirspeedMps", 10.0}, {"cruisePowerW", 100.0},
                        {"climb", {{"method", "potential-energy"}, {"efficiency", 0.5}}}};
    const json battery = {{"id", "b"}, {"cellsSeries", 1}, {"capacityAh", 1.0}, {"nominalCellVoltage", 10.0}, {"usableFraction", 1.0}};
    const LatLon a{48.5, 35.2};
    const LatLon b = world.direct(a, 90.0, 2000.0).point;
    json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 0.0}}},
                    {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", 0.0}}}},
                    {"returnToPilot", true}};
    json checks = json::array();
    bool ok = true;
    auto check = [&](const std::string& name, double got, double want, double tol) {
        const bool pass = std::abs(got - want) <= tol;
        checks.push_back({{"name", name}, {"ok", pass}, {"detail", "got " + std::to_string(got) + ", want " + std::to_string(want)}});
        ok = ok && pass;
    };
    // Calm: 4 km at 10 m/s = 400 s at 100 W = 11.111 Wh.
    json r = analyze({{"mission", mission}}, world, drone, battery);
    check("calm air energy", r.at("data").at("neededWh").get<double>(), 100.0 * 400.0 / 3600.0, 1e-6);
    // 5 m/s wind from the west: out at 15 m/s, back at 5 m/s → 2000/15 + 2000/5 = 533.33 s.
    r = analyze({{"mission", mission}, {"options", {{"windSpeedMps", 5.0}, {"windFromDeg", 270.0}}}}, world, drone, battery);
    check("head and tail wind", r.at("data").at("neededWh").get<double>(), 100.0 * (2000.0 / 15.0 + 2000.0 / 5.0) / 3600.0, 1e-4);
    // Crosswind 6 m/s from the north: ground speed sqrt(100 - 36) = 8 m/s both ways.
    r = analyze({{"mission", mission}, {"options", {{"windSpeedMps", 6.0}, {"windFromDeg", 0.0}}}}, world, drone, battery);
    check("crosswind", r.at("data").at("neededWh").get<double>(), 100.0 * 4000.0 / 8.0 / 3600.0, 1e-3);
    return {{"ok", ok}, {"checks", checks}};
}

}  // namespace fpv::battery

RFMOD_DEFINE_MODULE(fpv::battery::BatteryModule)
