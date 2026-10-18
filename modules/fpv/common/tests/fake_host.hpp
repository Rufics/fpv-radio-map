#pragma once

// The rfmap host, faked for the FPV modules' unit tests. It answers the host calls a module makes
// (geodesy, terrain, link margins, library items, weather) from a LocalWorld and fixed tables,
// so the code that ships (module.cpp, HostWorld, HostLinkEvaluator) runs in the tests through the
// same C ABI the server uses.

#include <fpv/mission.hpp>
#include <fpv/world.hpp>
#include <rfmod/abi.h>
#include <rfmod/module.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fpv::test {
class FakeHost;
}

/// What rfmod_host::context points to in the tests. Opaque to the module, as in the server.
struct rfmod_host_context {
    fpv::test::FakeHost* host = nullptr;
};

/// The entry point RFMOD_DEFINE_MODULE exports from the module.cpp linked into the test binary.
extern "C" RFMOD_EXPORT const rfmod_module* rfmod_get_module(void);

namespace fpv::test {

class FakeHost {
public:
    using Handler = std::function<json(const json&)>;

    /// Answers geodesy and terrain from `world`; elevation.point and profile.get report missing
    /// data where the world has none.
    explicit FakeHost(const LocalWorld& world) : world_(world) {
        context_.host = this;
        abi_ = {RFMOD_ABI_VERSION, &context_, &FakeHost::call, &FakeHost::log};
        on("geodesy.inverse", [this](const json& r) { return inverse(r); });
        on("geodesy.direct", [this](const json& r) { return direct(r); });
        on("elevation.point", [this](const json& r) { return elevation(r); });
        on("profile.get", [this](const json& r) { return profile(r); });
    }
    FakeHost(const FakeHost&) = delete;
    FakeHost& operator=(const FakeHost&) = delete;
    FakeHost(FakeHost&&) = delete;
    FakeHost& operator=(FakeHost&&) = delete;
    ~FakeHost() = default;

    /// Answers `method` with `handler`; a handler that throws makes the call fail.
    void on(const std::string& method, Handler handler) { handlers_[method] = std::move(handler); }

    /// link.evaluate: `atPilotDb` minus 1 dB per 100 m between the ground and the drone end.
    void linkByDistance(double atPilotDb) {
        on("link.evaluate", [this, atPilotDb](const json& r) {
            const json& ground = r.contains("ground") ? r.at("ground") : r.at("mission").at("pilot");
            const double d = world_.inverse(latLon(ground), latLon(r.at("drone"))).distanceM;
            return json{{"marginDb", atPilotDb - d / 100.0}};
        });
    }

    /// library.get answers `item` for (`kind`, `id`) and fails for anything else.
    void libraryItem(const std::string& kind, const std::string& id, json item) {
        library_[kind + "/" + id] = std::move(item);
        on("library.get", [this](const json& r) {
            const auto it = library_.find(r.at("kind").get<std::string>() + "/" + r.at("id").get<std::string>());
            if (it == library_.end()) throw std::out_of_range("not in the library");
            return it->second;
        });
    }

    [[nodiscard]] const rfmod_host* abi() const { return &abi_; }
    [[nodiscard]] int calls(const std::string& method) const {
        const auto it = counts_.find(method);
        return it == counts_.end() ? 0 : it->second;
    }
    [[nodiscard]] const std::vector<json>& requests() const { return requests_; }
    [[nodiscard]] const std::vector<std::string>& logLines() const { return logs_; }

    static LatLon latLon(const json& p) { return {p.at("lat").get<double>(), p.at("lon").get<double>()}; }

private:
    static int call(rfmod_host_context* self, const char* method, const char* request, const char** response) {
        FakeHost& host = *self->host;
        const std::string name = method ? method : "";
        ++host.counts_[name];
        try {
            const json req = json::parse(request ? request : "{}");
            host.requests_.push_back({{"method", name}, {"request", req}});
            const auto it = host.handlers_.find(name);
            if (it == host.handlers_.end()) throw std::invalid_argument("no handler for " + name);
            host.last_ = it->second(req).dump();
            *response = host.last_.c_str();
            return RFMOD_OK;
        } catch (const std::exception& e) {
            host.last_ = e.what();
            *response = host.last_.c_str();
            return RFMOD_ERR_HOST_CALL;
        }
    }

    static void log(rfmod_host_context* self, int level, const char* message) {
        self->host->logs_.push_back(std::to_string(level) + " " + (message ? message : ""));
    }

    [[nodiscard]] json inverse(const json& r) const {
        const InverseResult inv = world_.inverse(latLon(r.at("from")), latLon(r.at("to")));
        return {{"distanceM", inv.distanceM}, {"azimuth1Deg", inv.azimuth1Deg}, {"azimuth2Deg", inv.azimuth2Deg}};
    }

    [[nodiscard]] json direct(const json& r) const {
        const DirectResult d = world_.direct(latLon(r.at("from")), r.at("azimuthDeg").get<double>(), r.at("distanceM").get<double>());
        return {{"lat", d.point.lat}, {"lon", d.point.lon}, {"azimuthDeg", d.azimuthDeg}};
    }

    [[nodiscard]] json elevation(const json& r) const {
        try {
            return {{"groundMslM", world_.groundMsl(latLon(r))}};
        } catch (const MissingTerrain&) {
            return {{"groundMslM", nullptr}};
        }
    }

    [[nodiscard]] json profile(const json& r) const {
        try {
            json samples = json::array();
            for (const GroundSample& s : world_.groundProfile(latLon(r.at("from")), latLon(r.at("to")))) {
                samples.push_back({{"distanceM", s.distanceM}, {"groundMslM", s.groundMslM}, {"lat", s.position.lat},
                                   {"lon", s.position.lon}});
            }
            return {{"samples", samples}, {"missing", json::array()}};
        } catch (const MissingTerrain&) {
            return {{"samples", json::array()}, {"missing", {{{"fromM", 0.0}}}}};
        }
    }

    const LocalWorld& world_;
    rfmod_host_context context_;
    rfmod_host abi_{};
    std::map<std::string, Handler> handlers_;
    std::map<std::string, json> library_;
    std::map<std::string, int> counts_;
    std::vector<json> requests_;
    std::vector<std::string> logs_;
    std::string last_;
};

/// The module linked into the test binary, driven through its C ABI like the server does.
class ModuleUnderTest {
public:
    struct Result {
        int status;
        json body;
    };

    ModuleUnderTest() : api_(rfmod_get_module()) {}

    [[nodiscard]] json manifest() const { return json::parse(api_->manifest_json()); }
    [[nodiscard]] int init(const FakeHost& host, const std::string& options = "") const {
        return api_->init(host.abi(), options.c_str());
    }
    [[nodiscard]] int initWith(const rfmod_host* host) const { return api_->init(host, ""); }

    [[nodiscard]] Result invoke(const std::string& method, const std::string& request) const {
        char* out = nullptr;
        const int status = api_->invoke(method.c_str(), request.c_str(), &out);
        Result r{status, out ? json::parse(out) : json()};
        if (out) api_->free_string(out);
        return r;
    }
    [[nodiscard]] Result invoke(const std::string& method, const json& request) const { return invoke(method, request.dump()); }
    [[nodiscard]] Result analyze(const json& request) const { return invoke("route.analyze", request); }
    [[nodiscard]] Result selftest() const { return invoke("rfmod.selftest", std::string()); }
    void shutdown() const { api_->shutdown(); }

private:
    const rfmod_module* api_;
};

/// Flat ground at 100 m MSL, sampled every 30 m.
inline LocalWorld flatWorld() {
    return LocalWorld([](const LatLon&) { return std::optional<double>(100.0); }, 30.0);
}

/// A mission from (48.5, 35.2) to one waypoint `km` due east.
inline json eastMission(const IWorld& world, double km, double aglM, const std::string& evaluate = "control",
                        bool returnToPilot = false) {
    const LatLon a{48.5, 35.2};
    const LatLon b = world.direct(a, 90.0, km * 1000.0).point;
    return {{"pilot", {{"lat", a.lat}, {"lon", a.lon}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", b.lat}, {"lon", b.lon}, {"aglM", aglM}}}},
            {"returnToPilot", returnToPilot},
            {"evaluate", evaluate}};
}

/// An area spanning `fromKm` to `toKm` due east of the pilot, `halfWidthDeg` north and south.
inline json eastArea(const IWorld& world, double fromKm, double toKm, double halfWidthDeg = 0.0005) {
    const LatLon a{48.5, 35.2};
    const double w = world.direct(a, 90.0, fromKm * 1000.0).point.lon;
    const double e = world.direct(a, 90.0, toKm * 1000.0).point.lon;
    const double s = a.lat - halfWidthDeg, n = a.lat + halfWidthDeg;
    return {{{"lat", s}, {"lon", w}}, {{"lat", n}, {"lon", w}}, {{"lat", n}, {"lon", e}}, {{"lat", s}, {"lon", e}}};
}

/// A test of a module's entry file: the module linked into the binary, initialised with a fresh
/// fake host over a terrain the test can change (flat ground at 100 m MSL until it does).
class ModuleEntryTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(module_.init(host_), RFMOD_OK); }

    /// What every module's entry must do: report its manifest, pass its self-test, and turn
    /// unknown methods, malformed requests and bad host handles into ABI errors.
    void expectStandardEntry(const std::string& id) const {
        EXPECT_EQ(module_.manifest().at("id"), id);
        const auto self = module_.selftest();
        EXPECT_EQ(self.status, RFMOD_OK);
        EXPECT_TRUE(self.body.at("ok").get<bool>()) << self.body.dump();

        const auto unknown = module_.invoke("route.nothing", json::object());
        EXPECT_EQ(unknown.status, RFMOD_ERR_UNKNOWN_METHOD);
        EXPECT_EQ(unknown.body.at("error").at("code"), "unknown_method");
        EXPECT_EQ(module_.invoke("route.analyze", std::string("{not json")).status, RFMOD_ERR_BAD_REQUEST);
        const auto noMission = module_.analyze(json::object());
        EXPECT_EQ(noMission.status, RFMOD_ERR_BAD_REQUEST);
        EXPECT_EQ(noMission.body.at("error").at("code"), "bad_request");

        rfmod_host wrongVersion = *host_.abi();
        wrongVersion.abi_version = RFMOD_ABI_VERSION + 1;
        EXPECT_EQ(module_.initWith(&wrongVersion), RFMOD_ERR_BAD_REQUEST);
        EXPECT_EQ(module_.initWith(nullptr), RFMOD_ERR_BAD_REQUEST);
        EXPECT_EQ(module_.init(host_, "{broken"), RFMOD_ERR_INTERNAL);
        EXPECT_EQ(module_.init(host_, R"({"any":1})"), RFMOD_OK);
        module_.shutdown();
    }

    /// Makes every host call to `method` fail, as when the server cannot answer it.
    void failHostCall(const std::string& method) {
        host_.on(method, [method](const json&) -> json { throw std::runtime_error(method + " is down"); });
    }

    std::function<std::optional<double>(const LatLon&)> terrain_ = [](const LatLon&) { return std::optional<double>(100.0); };
    LocalWorld world_{[this](const LatLon& p) { return terrain_(p); }, 30.0};
    FakeHost host_{world_};
    ModuleUnderTest module_;
};

}  // namespace fpv::test
