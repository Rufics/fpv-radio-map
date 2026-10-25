// fpv.airframe module: host glue and self-test.

#include "envelope.hpp"

#include <cmath>

namespace fpv::airframe {
namespace {

constexpr const char* kId = "fpv.airframe";
constexpr const char* kManifest = R"json({"id":"fpv.airframe","version":"1.0.0"})json";

}  // namespace

class AirframeModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const Mission m = Mission::fromJson(request.at("mission"));
        if (m.droneId.empty()) {
            return {{"reasons", {reason(kId, "info", "Pick a drone to check the route against its airframe.")}}, {"data", json::object()}};
        }
        const json drone = host().call("library.get", {{"kind", "drones"}, {"id", m.droneId}});
        const HostWorld world(host());
        return analyze(request, world, drone);
    }

    json selftest() override;
};

json AirframeModule::selftest() {
    // Flat ground at 100 m; 25° bank at 20 m/s is an 87.5 m radius; a 10° climb to 120 m over
    // 400 m falls 120 - 1.5 - 400 tan 10° = 48 m short.
    const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 20.0);
    const json drone = {{"id", "wing"}, {"airframe", "fixed-wing"}, {"cruiseAirspeedMps", 20.0},
                        {"fixedWing", {{"stallSpeedMps", 11.0}, {"maxBankDeg", 25.0}, {"maxClimbAngleDeg", 10.0}, {"maxDiveAngleDeg", 10.0}}}};
    const LatLon a{48.5, 35.2};
    const LatLon b = world.direct(a, 90.0, 400.0).point;
    const LatLon c = world.direct(b, 0.0, 2000.0).point;
    const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                          {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", 120.0}}, {{"lat", c.lat}, {"lon", c.lon}, {"aglM", 120.0}}}},
                          {"returnToPilot", false}};
    const json r = analyze({{"mission", mission}}, world, drone);
    const json& data = r.at("data");
    const double radius = data.at("turns").at(0).at("radiusM").get<double>();
    const double below = data.at("vertical").at("arrivals").at(0).at("belowPlanM").get<double>();
    const bool radiusOk = std::abs(radius - 400.0 / (9.80665 * std::tan(25.0 * rfcore::constants::kPi / 180.0))) < 1e-6;
    const bool climbOk = std::abs(below - (118.5 - 400.0 * std::tan(10.0 * rfcore::constants::kPi / 180.0))) < 0.5;
    return {{"ok", radiusOk && climbOk},
            {"checks", {{{"name", "turn radius V^2 / (g tan bank)"}, {"ok", radiusOk}, {"detail", std::to_string(radius)}},
                        {{"name", "climb limited to the envelope's angle"}, {"ok", climbOk}, {"detail", std::to_string(below)}}}}};
}

}  // namespace fpv::airframe

RFMOD_DEFINE_MODULE(fpv::airframe::AirframeModule)
