// fpv.altitude module: host glue and self-test.

#include "auto_height.hpp"
#include "climb_assistant.hpp"

#include <cmath>
#include <optional>

namespace fpv::altitude {
namespace {
constexpr const char* kManifest = R"json({"id":"fpv.altitude","version":"1.2.0"})json";
}  // namespace

class AltitudeModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze" && method != "route.prepare") throw rfmod::UnknownMethod(method);
        const Mission m = Mission::fromJson(request.at("mission"));
        const LinkInputs links = readLinks(host(), m);
        const HostWorld world(host());
        if (method == "route.prepare") return prepare(request, world, links);
        // The climb assistant estimates energy for the mission's drone, in the forecast wind.
        // The forecast goes to the assistant only: the corridor's refraction keeps to the request.
        std::optional<json> drone;
        json climbWeather = request.value("weather", json());
        // Without the drone the assistant still runs, without energy; without a forecast, in still air.
        // With the assistant off neither is needed.
        const bool climbAssist = ClimbSettings::fromOptions(request.value("options", json::object())).enabled;
        if (climbAssist && !m.droneId.empty()) {
            try {
                drone = host().call("library.get", {{"kind", "drones"}, {"id", m.droneId}});
            } catch (const rfmod::HostError&) {
                drone.reset();
            }
        }
        if (drone && climbWeather.is_null()) {
            try {
                climbWeather = host().call("weather.point", {{"lat", m.pilot.position.lat}, {"lon", m.pilot.position.lon},
                                                             {"time", request.value("time", "")}});
            } catch (const rfmod::HostError&) {
                climbWeather = nullptr;
            }
        }
        return analyze(request, world, links, drone, climbWeather);
    }

    json selftest() override {
        // Flat ground at 100 m and a 1 km leg climbing to 50 m above launch: past a 600 m take-off
        // zone the corridor is 130 to 220 m MSL, and the leg is over 130 m from 588 m out, so it
        // stays inside it.
        const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 10.0);
        const LatLon a{48.5, 35.2};
        const LatLon b = world.direct(a, 90.0, 1000.0).point;
        const json mission = {{"altitudeRef", "launch"},
                              {"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                              {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"altM", 50.0}}}},
                              {"returnToPilot", false}};
        const json r = analyze({{"mission", mission}, {"options", {{"takeoffM", 600.0}, {"climbAssist", false}}}}, world, {});
        const json& c = r.at("data").at("corridor");
        const bool floor = std::abs(c.at("floorMslM").back().get<double>() - 130.0) < 1e-6;
        const bool ceiling = std::abs(c.at("ceilingMslM").back().get<double>() - 220.0) < 1e-6;
        const bool planned = std::abs(c.at("plannedMslM").back().get<double>() - 150.0) < 1e-6;
        const bool quiet = r.at("reasons").empty();
        return {{"ok", floor && ceiling && planned && quiet},
                {"checks", {{{"name", "floor is ground + 30 m"}, {"ok", floor}},
                            {{"name", "ceiling is ground + 120 m"}, {"ok", ceiling}},
                            {{"name", "planned is launch + 50 m"}, {"ok", planned}},
                            {{"name", "no warning: the low take-off is in the take-off zone"}, {"ok", quiet}}}}};
    }
};

}  // namespace fpv::altitude

RFMOD_DEFINE_MODULE(fpv::altitude::AltitudeModule)
