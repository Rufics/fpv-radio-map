// fpv.core module: host glue and self-test.

#include "core.hpp"

#include <cmath>

namespace fpv::core {
namespace {

constexpr const char* kManifest = R"json({"id":"fpv.core","version":"1.0.0"})json";

/// Self-test link: margin falls 1 dB per 100 m, and a mission field "gainDb" adds to it.
class DistanceLink final : public ILinkEvaluator {
public:
    DistanceLink(const IWorld& w, double gainDb) : w_(w), gainDb_(gainDb) {}
    double marginDb(const std::string&, const End& g, const End& d) const override {
        return 10.0 + gainDb_ - w_.inverse(g.position, d.position).distanceM / 100.0;
    }

private:
    const IWorld& w_;
    double gainDb_;
};

}  // namespace

class CoreModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const HostWorld world(host());
        return analyze(request, world, [this](const json& m) -> std::unique_ptr<ILinkEvaluator> {
            return std::make_unique<HostLinkEvaluator>(host(), m);
        });
    }

    json selftest() override {
        const LocalWorld world([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
        const LatLon a{48.5, 35.2};
        const LatLon t = world.direct(a, 90.0, 1000.0).point;
        const json mission = {{"name", "Test & <check>"},
                              {"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
                              {"waypoints", {{{"lat", t.lat}, {"lon", t.lon}, {"aglM", 50.0}}}},
                              {"returnToPilot", false}, {"evaluate", "control"}, {"gainDb", 0.0}};
        const json r = analyze({{"mission", mission},
                                {"options", {{"compare", {{"name", "B"}, {"patch", {{"gainDb", 3.0}}}}},
                                             {"stepM", 500.0}, {"export", {"kml", "gpx"}}}}},
                               world, [&world](const json& m) -> std::unique_ptr<ILinkEvaluator> {
                                   return std::make_unique<DistanceLink>(world, m.value("gainDb", 0.0));
                               });
        const json& c = r.at("data").at("compare").at("links").at("control");
        const bool cmp = std::abs(c.at("deltaWorstDb").get<double>() - 3.0) < 1e-9 && c.at("pointsBetter") == 2;
        const std::string kml = r.at("data").at("export").at("kml").at("content");
        const std::string gpx = r.at("data").at("export").at("gpx").at("content");
        const bool exp = kml.find("Test &amp; &lt;check&gt;") != std::string::npos &&
                         gpx.find("<ele>101.500</ele>") != std::string::npos;
        return {{"ok", cmp && exp},
                {"checks", {{{"name", "scenario comparison"}, {"ok", cmp}, {"detail", c.at("deltaWorstDb").dump()}},
                            {{"name", "KML and GPX export"}, {"ok", exp}}}}};
    }
};

}  // namespace fpv::core

RFMOD_DEFINE_MODULE(fpv::core::CoreModule)
