// fpv.failsafe module: host glue and self-test.

#include "failsafe.hpp"

#include <cmath>

namespace fpv::failsafe {
namespace {

constexpr const char* kManifest = R"json({"id":"fpv.failsafe","version":"1.0.0"})json";

/// Self-test probe: control margin falls 1 dB per 100 m from a fixed point.
class DistanceProbe final : public ILinkProbe {
public:
    DistanceProbe(const IWorld& w, LatLon home, double zeroAtM) : w_(w), home_(home), zero_(zeroAtM) {}
    double marginDb(const LatLon& p, double) const override { return (zero_ - w_.inverse(home_, p).distanceM) / 100.0; }

private:
    const IWorld& w_;
    LatLon home_;
    double zero_;
};

}  // namespace

class FailsafeModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const Mission m = Mission::fromJson(request.at("mission"));
        std::optional<json> drone;
        std::optional<json> battery;
        if (!m.droneId.empty() && !m.batteryId.empty()) {
            drone = host().call("library.get", {{"kind", "drones"}, {"id", m.droneId}});
            battery = host().call("library.get", {{"kind", "batteries"}, {"id", m.batteryId}});
        }
        const HostWorld world(host());
        const HostLinkProbe probe(host(), request.at("mission"));
        return analyze(request, world, probe, drone, battery);
    }

    json selftest() override {
        const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
        const LatLon a{48.5, 35.2};
        const LatLon b = world.direct(a, 90.0, 3000.0).point;
        const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                              {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", 50.0}}}},
                              {"returnToPilot", true}, {"evaluate", "control"}};
        const DistanceProbe probe(world, a, 2000.0);
        const json r = analyze({{"mission", mission}, {"options", {{"stepM", 100.0}}}}, world, probe, std::nullopt, std::nullopt);
        const double at = r.at("data").at("loss").at("distanceAlongRouteM").get<double>();
        const bool lossOk = std::abs(at - 2000.0) <= 1.0;
        const bool regains = r.at("data").at("rth").value("regainAfterM", -1.0) >= 0.0;
        return {{"ok", lossOk && regains},
                {"checks", {{{"name", "loss found within 1 m"}, {"ok", lossOk}, {"detail", std::to_string(at)}},
                            {{"name", "link back on the way home"}, {"ok", regains}}}}};
    }
};

}  // namespace fpv::failsafe

RFMOD_DEFINE_MODULE(fpv::failsafe::FailsafeModule)
