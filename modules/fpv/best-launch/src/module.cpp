// fpv.best-launch module: host glue and self-test.

#include "best_launch.hpp"

#include <cmath>

namespace fpv::launch {
namespace {

constexpr const char* kManifest = R"json({"id":"fpv.best-launch","version":"1.0.0"})json";

/// Self-test link: margin 30 dB minus 1 dB per 100 m between the ends.
class DistanceLink final : public ILinkEvaluator {
public:
    explicit DistanceLink(const IWorld& w) : w_(w) {}
    double marginDb(const std::string&, const End& g, const End& d) const override {
        return 30.0 - w_.inverse(g.position, d.position).distanceM / 100.0;
    }

private:
    const IWorld& w_;
};

}  // namespace

class BestLaunchModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const HostWorld world(host());
        const HostLinkEvaluator eval(host(), request.at("mission"));
        return analyze(request, world, eval);
    }

    json selftest() override {
        // Target 2 km east, route checked at 0.5, 1, 1.5 and 2 km; the area spans 0.5-1.5 km east.
        // The worst sample is the farthest one, so the best spot is the minimax point 1.25 km out,
        // 750 m from the target (within half a 100 m cell and the area's 110 m width).
        const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
        const LatLon a{48.5, 35.2};
        const LatLon t = world.direct(a, 90.0, 2000.0).point;
        const LatLon w0 = world.direct(a, 90.0, 500.0).point;
        const LatLon w1 = world.direct(a, 90.0, 1500.0).point;
        const json area = {{{"lat", 48.499}, {"lon", w0.lon}}, {{"lat", 48.501}, {"lon", w0.lon}},
                           {{"lat", 48.501}, {"lon", w1.lon}}, {{"lat", 48.499}, {"lon", w1.lon}}};
        const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                              {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
                              {"returnToPilot", false}, {"evaluate", "control"}};
        const DistanceLink link(world);
        const json r = analyze({{"mission", mission}, {"options", {{"area", area}, {"cellM", 100.0}, {"stepM", 500.0}}}}, world, link);
        const json& best = r.at("data").at("candidates").at(0);
        const double fromTarget = world.inverse({best.at("lat"), best.at("lon")}, t).distanceM;
        const bool ok = std::abs(fromTarget - 750.0) < 80.0 && r.at("reasons").at(0).contains("fix");
        return {{"ok", ok}, {"checks", {{{"name", "best point is the minimax point"}, {"ok", ok}, {"detail", std::to_string(fromTarget)}}}}};
    }
};

}  // namespace fpv::launch

RFMOD_DEFINE_MODULE(fpv::launch::BestLaunchModule)
