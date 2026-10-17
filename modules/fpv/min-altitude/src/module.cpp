// fpv.min-altitude module: host glue and self-test.

#include "analysis.hpp"

#include <cmath>

namespace fpv::minalt {
namespace {
constexpr const char* kManifest = R"json({"id":"fpv.min-altitude","version":"1.0.0"})json";
}  // namespace

class MinAltitudeModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    json invoke(const std::string& method, const json& request) override {
        if (method != "route.analyze") throw rfmod::UnknownMethod(method);
        const Mission m = Mission::fromJson(request.at("mission"));
        std::map<std::string, LinkInput> links;
        for (const std::string& kind : m.links()) {
            const json setup = m.raw.value("links", json::object()).value(kind, json());
            if (!setup.is_object()) continue;
            const json radio = host().call("library.get", {{"kind", "radio-profiles"}, {"id", setup.at("radio")}});
            links.emplace(kind, LinkInput{rfcore::Hertz(radio.at("frequencyHz").get<double>()), radio.value("name", "")});
        }
        const HostWorld world(host());
        return analyze(request, world, links);
    }

    json selftest() override {
        // Flat ground, no curvature effect worth noting over 1 km: the requirement is set by a
        // 30 m wall half way. Check the solver's answer satisfies the clearance and 1 mm less does not.
        const LocalWorld world(
            [](const LatLon& p) {
                const double d = std::abs(p.lon - 35.2) * 73800.0;  // rough metres east, only to place the wall
                return std::optional<double>(d > 480.0 && d < 520.0 ? 130.0 : 100.0);
            },
            10.0);
        const LatLon a{48.5, 35.2};
        const LatLon b = world.direct(a, 90.0, 1000.0).point;
        const auto inv = world.inverse(a, b);
        const auto mid = world.direct(a, inv.azimuth1Deg, inv.distanceM / 2.0);
        const auto profile = ClearanceSolver::toProfile(world.groundProfile(a, b), mid.azimuthDeg, mid.point.lat);
        const ClearanceSolver solver(4.0 / 3.0, rfcore::Hertz(5.8e9), 0.6);
        const auto h = solver.requiredAgl(profile, 2.0, 500.0);
        const bool found = h.has_value();
        const bool clears = found && solver.clearance(profile, 2.0, *h) >= 0.6;
        const bool tight = found && solver.clearance(profile, 2.0, *h - 2e-3) < 0.6;
        return {{"ok", found && clears && tight},
                {"checks", {{{"name", "a height is found"}, {"ok", found}},
                            {{"name", "it clears 60 %"}, {"ok", clears}},
                            {{"name", "2 mm lower does not"}, {"ok", tight}}}}};
    }
};

}  // namespace fpv::minalt

RFMOD_DEFINE_MODULE(fpv::minalt::MinAltitudeModule)
