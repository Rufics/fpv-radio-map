// fpv.relay module: host glue and self-test.

#include "relay.hpp"

#include <cmath>

namespace fpv::relay {
namespace {

constexpr const char* kManifest = R"json({"id":"fpv.relay","version":"1.0.0"})json";

class DistanceLink final : public ILinkEvaluator {
public:
    explicit DistanceLink(const IWorld& w) : w_(w) {}
    double marginDb(const std::string&, const End& g, const End& d) const override {
        return 10.0 - w_.inverse(g.position, d.position).distanceM / 100.0;
    }

private:
    const IWorld& w_;
};

}  // namespace

class RelayModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const HostWorld world(host());
        const HostLinkEvaluator eval(host(), request.at("mission"));
        return analyze(request, world, eval);
    }

    json selftest() override {
        // Margin 10 dB - 1 dB/100 m. Target 1.6 km out: direct worst -6 dB. A relay half way
        // (0.8 km) makes both hops 0.8 km long: worst +2 dB.
        const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
        const LatLon a{48.5, 35.2};
        const LatLon t = world.direct(a, 90.0, 1600.0).point;
        const LatLon mid = world.direct(a, 90.0, 800.0).point;
        const json mission = {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                              {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
                              {"returnToPilot", false}, {"evaluate", "control"}};
        const DistanceLink link(world);
        const json r = analyze({{"mission", mission}, {"options", {{"relay", {{"lat", mid.lat}, {"lon", mid.lon}}}, {"stepM", 400.0}}}},
                               world, link);
        const double direct = r.at("data").at("direct").at("worstMarginDb").get<double>();
        const double score = r.at("data").at("chosen").at("scoreDb").get<double>();
        const bool ok = std::abs(direct + 6.0) < 1e-6 && std::abs(score - 2.0) < 1e-6;
        return {{"ok", ok}, {"checks", {{{"name", "direct and relayed budgets"}, {"ok", ok},
                                         {"detail", std::to_string(direct) + " / " + std::to_string(score)}}}}};
    }
};

}  // namespace fpv::relay

RFMOD_DEFINE_MODULE(fpv::relay::RelayModule)
