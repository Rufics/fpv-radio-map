// Test module for the server's integration tests. Its "mode" option picks the answer: well-formed
// answers that also carry the odd entries a third-party module may send (an unknown severity, a
// leg that does not exist, a layer of the wrong size), a failure on every call, or a route answer
// that is not an object. The server must keep the good parts and report the rest.

#include <rfmod/module.hpp>

#include <string>
#include <vector>

namespace {

// Must match manifest.json; the host refuses the package otherwise.
constexpr const char* kManifest = R"json({"id":"test.probe","version":"1.0.0"})json";

class ProbeModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }

    void init(const rfmod::HostClient&, const rfmod::json&) override {}

    rfmod::json invoke(const std::string& method, const rfmod::json& request) override {
        const std::string mode = request.value("options", rfmod::json::object()).value("mode", "answer");
        if (mode == "fail") throw rfmod::BadRequest("probe asked to fail");
        if (method == "loss.contribute") return contribute();
        if (method == "route.prepare") return mode == "not-an-object" ? rfmod::json::array() : prepareRoute(request.at("mission"), mode);
        if (method == "route.analyze") return mode == "not-an-object" ? rfmod::json::array() : analyzeRoute();
        if (method == "coverage.analyze") return analyzeCoverage(request.at("grid"));
        throw rfmod::UnknownMethod(method);
    }

    rfmod::json selftest() override {
        const bool ok = contribute().at("terms").at(0).at("valueDb").get<double>() == 1.0;
        return {{"ok", ok}, {"checks", {{{"name", "term is 1 dB"}, {"ok", ok}}}}};
    }

private:
    static rfmod::json contribute() {
        return {{"terms", {{{"id", "test.probe/term"}, {"label", "Probe term"}, {"valueDb", 1.0}, {"source", "test"}}}},
                {"notes", rfmod::json::array()}};
    }

    /// 77 m for every waypoint, the manual ones too (the server keeps those), or one that does not exist.
    static rfmod::json prepareRoute(const rfmod::json& mission, const std::string& mode) {
        const std::string field = mission.value("altitudeRef", "agl") == "launch" ? "altM" : "aglM";
        rfmod::json heights = rfmod::json::array();
        for (std::size_t i = 0; i < mission.at("waypoints").size(); ++i) {
            heights.push_back({{"index", i}, {"field", field}, {"heightM", 77.0}, {"limitedBy", "probe"}, {"blocked", i == 0}});
        }
        if (mode == "bad-height") heights.push_back({{"index", 99}, {"heightM", 1.0}});
        if (mode == "bad-type") heights[0]["blocked"] = "yes";
        if (mode == "bad-list") return {{"waypoints", {{"first", heights[0]}}}};
        return {{"waypoints", heights}, {"reasons", {{{"severity", "warn"}, {"text", "probe set heights"}, {"leg", 0}}}}};
    }

    static rfmod::json analyzeRoute() {
        return {{"reasons",
                 {{{"severity", "warn"}, {"text", "on the first leg"}, {"leg", 0}},
                  {{"severity", "fatal"}, {"text", "unknown severity"}},
                  {{"severity", "info"}, {"text", "on a leg that does not exist"}, {"leg", 99}},
                  {{"severity", "info"}, {"text", "leg is not a number"}, {"leg", "first"}},
                  "not an object"}},
                {"data", {{"probe", true}}},
                {"battery", {{"source", "test.probe"}}}};
    }

    static rfmod::json analyzeCoverage(const rfmod::json& grid) {
        const auto cells = grid.value("cols", 0) * grid.value("rows", 0);
        return {{"reasons", {{{"severity", "info"}, {"text", "probe looked at the grid"}}, 42}},
                {"layers",
                 {{{"id", "fits"}, {"values", rfmod::json(std::vector<double>(cells, 1.0))}},
                  {{"id", "too-short"}, {"values", {1.0}}},
                  {{"id", "no-values"}},
                  "not an object"}},
                {"data", {{"cells", cells}}}};
    }
};

}  // namespace

RFMOD_DEFINE_MODULE(ProbeModule)
