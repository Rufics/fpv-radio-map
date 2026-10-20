// End to end: the server against a local PostGIS cluster (ctest fixture), a synthetic DEM
// COG written with GDAL, a stand-in Open-Meteo, and the example body-shadow module.

#include "app.hpp"
#include "test_support.hpp"

#include <rfcore/constants.hpp>
#include <rfmod/package.hpp>

#include <httplib.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

// Synthetic terrain: flat 150 m with a north-south ridge 120 m high at lon 35.25, and a NoData
// hole in the south-east corner. 3 arc-second pixels.
constexpr double kWest = 35.15, kNorth = 48.60, kPx = 1.0 / 1200.0;
constexpr int kCols = 240, kRows = 240;

double terrain(int c, int r) {
    const double lon = kWest + (c + 0.5) * kPx;
    const double lat = kNorth - (r + 0.5) * kPx;
    if (lat < 48.45 && lon > 35.33) return NAN;
    const double x = (lon - 35.25) / 0.004;
    return 150.0 + 120.0 * std::exp(-x * x);
}

// Land cover: ESA WorldCover "tree cover" (10) in a strip between lon 35.21 and 35.23, no data
// (0) elsewhere.
double landcover(int c, int) {
    const double lon = kWest + (c + 0.5) * kPx;
    return lon > 35.21 && lon < 35.23 ? 10.0 : 0.0;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// Stand-in for a self-hosted Open-Meteo: 72 hours from yesterday, rain 2 mm/h, a standard-ish
/// column. The 1000 hPa level lies below the ground and must be ignored.
class FakeOpenMeteo {
public:
    FakeOpenMeteo() {
        server_.Get("/v1/forecast", [this](const httplib::Request& req, httplib::Response& res) {
            ++requests;
            lastQuery = req.params.contains("hourly") ? req.get_param_value("hourly") : "";
            const auto now = std::chrono::floor<std::chrono::hours>(Clock::now());
            const long long t0 = Clock::to_time_t(now - std::chrono::hours(24));
            json h = {{"time", json::array()}};
            const std::vector<std::pair<int, double>> levels = {{1000, 110.0}, {975, 330.0}, {950, 545.0},
                                                                {925, 765.0}, {900, 990.0}, {850, 1460.0}};
            for (int i = 0; i < 72; ++i) {
                h["time"].push_back(t0 + 3600LL * i);
                h["temperature_2m"].push_back(15.0);
                h["relative_humidity_2m"].push_back(60.0);
                h["surface_pressure"].push_back(995.0);
                h["precipitation"].push_back(2.0);
                h["wind_speed_10m"].push_back(4.0);
                h["wind_direction_10m"].push_back(270.0);
                for (const auto& [p, gh] : levels) {
                    const std::string s = std::to_string(p) + "hPa";
                    h["temperature_" + s].push_back(15.0 - 0.0065 * (gh - 150.0));
                    h["relative_humidity_" + s].push_back(55.0);
                    h["geopotential_height_" + s].push_back(gh);
                }
            }
            res.set_content(json{{"latitude", 48.5}, {"longitude", 35.2}, {"elevation", 150.0}, {"hourly", h}}.dump(),
                            "application/json");
        });
        port = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
    }
    ~FakeOpenMeteo() {
        server_.stop();
        thread_.join();
    }
    int port = 0;
    std::atomic<int> requests{0};
    std::string lastQuery;

private:
    httplib::Server server_;
    std::thread thread_;
};

struct Env {
    fs::path dir;
    std::unique_ptr<FakeOpenMeteo> weather;
    std::unique_ptr<Application> app;
    std::thread thread;
    int port = 0;
    std::string missionId, ownerKey, editSession, viewSession, linkId, linkToken;
};
Env* env = nullptr;

/// RFMAP_IT_DUMP_DIR=<dir> keeps chosen responses for inspection.
void dump(const std::string& name, const json& body) {
    if (const char* d = std::getenv("RFMAP_IT_DUMP_DIR")) {
        std::ofstream(fs::path(d) / (name + ".json")) << body.dump(2) << "\n";
    }
}

class Integration : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const char* url = std::getenv("RFMAP_TEST_DATABASE_URL");
        if (!url) return;
        env = new Env;
        env->dir = rfmap::test::scratchDir("integration") / ("rfmap-it-" + std::to_string(::getpid()));
        fs::remove_all(env->dir);
        fs::create_directories(env->dir);
        rfmap::test::writeCog(env->dir / "dem.cog.tif", kWest, kNorth, kPx, kCols, kRows, terrain);
        rfmap::test::writeCog(env->dir / "landcover.tif", kWest, kNorth, kPx, kCols, kRows, landcover);
        env->weather = std::make_unique<FakeOpenMeteo>();

        // The database is fresh per ctest run (fixture); empty it anyway so reruns work.
        {
            pqxx::connection c(url);
            pqxx::work w(c);
            w.exec("drop schema public cascade; create schema public;");
            w.commit();
        }
        json cfg = {{"server", {{"listen", "127.0.0.1"}, {"threads", 8}}},
                    {"database", {{"url", url}, {"poolSize", 4}}},
                    {"admin", {{"token", "integration-admin-token-0123456789"}}},
                    {"data",
                     {{"dem", (env->dir / "dem.cog.tif").string()},
                      {"demStatedErrorM", 4.0},
                      {"demStatedErrorSource", "synthetic test terrain, stated LE90"},
                      {"demName", "Synthetic ridge 3\""},
                      {"landcover", (env->dir / "landcover.tif").string()},
                      {"landcoverName", "Synthetic tree strip"}}},
                    {"schemasDir", RFMAP_SCHEMAS_DIR},
                    {"seedDir", RFMAP_SEED_DIR},
                    {"modules",
                     {{"root", (env->dir / "modules").string()},
                      {"worker", RFMOD_WORKER},
                      {"trustStore", (env->dir / "keys.json").string()}}},
                    {"weather", {{"baseUrl", "http://127.0.0.1:" + std::to_string(env->weather->port)}}},
                    {"auth", {{"argon2Cost", "interactive"}, {"pinLockoutMinutes", 15}}},
                    {"coverage", {{"threads", 4}, {"maxCells", 20000}}},
                    // Generous limits: the test makes many requests from one address.
                    {"rateLimits", {{"default", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"calc", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"admin", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"open", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"create", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"coverage", {{"perMinute", 6000}, {"burst", 1000}}}}}};
        env->app = std::make_unique<Application>(Config::fromJson(cfg), readFile(RFMOD_TEST_KEYS "/dev-owner.pub"), true,
                                                 RFMAP_SQL_DIR);
        env->port = env->app->bindAny();
        env->thread = std::thread([] { env->app->serveBound(); });
    }

    static void TearDownTestSuite() {
        if (!env) return;
        env->app->stop();
        env->thread.join();
        env->app.reset();
        env->weather.reset();
        fs::remove_all(env->dir);
        delete env;
        env = nullptr;
    }

    void SetUp() override {
        if (!env) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
    }

    struct Reply {
        int status;
        json body;
        httplib::Headers headers;
    };

    static Reply call(const std::string& method, const std::string& path, const json& body = nullptr,
                      const httplib::Headers& headers = {}) {
        httplib::Client c("127.0.0.1", env->port);
        c.set_read_timeout(120, 0);
        httplib::Result r;
        const std::string url = "/api/v1" + path;
        const std::string b = body.is_null() ? "" : body.dump();
        if (method == "GET") r = c.Get(url, headers);
        else if (method == "POST") r = c.Post(url, headers, b, "application/json");
        else if (method == "PUT") r = c.Put(url, headers, b, "application/json");
        else if (method == "DELETE") r = c.Delete(url, headers);
        if (!r) throw std::runtime_error("no response for " + method + " " + path);
        json parsed;
        if (!r->body.empty()) {
            try {
                parsed = json::parse(r->body);
            } catch (...) {
                parsed = r->body;
            }
        }
        return {r->status, parsed, r->headers};
    }
    static httplib::Headers bearer(const std::string& t) { return {{"Authorization", "Bearer " + t}}; }
    static httplib::Headers admin() { return bearer("integration-admin-token-0123456789"); }
    static httplib::Headers owner() { return {{"X-Owner-Key", env->ownerKey}}; }

    static json point(double lat, double lon, double agl) { return {{"lat", lat}, {"lon", lon}, {"aglM", agl}}; }
    static json controlLink() {
        return {{"radio", "elrs-915-50hz"}, {"txPowerDbm", 27}, {"groundAntenna", "test-ground-dipole"},
                {"airAntenna", "test-air-dipole"}, {"requiredMarginDb", 10}};
    }
    static json videoLink() {
        return {{"radio", "test-video-5g8"}, {"txPowerDbm", 25}, {"groundAntenna", "test-ground-dipole"},
                {"airAntenna", "test-air-dipole"}, {"requiredMarginDb", 6}};
    }
    static json mission() {
        return {{"schema", "mission/v1"},
                {"name", "Ridge test"},
                {"time", {{"mode", "now"}}},
                {"pilot", point(48.50, 35.20, 1.5)},
                {"waypoints", {point(48.52, 35.22, 120), point(48.50, 35.30, 60)}},
                {"returnToPilot", true},
                {"links", {{"control", controlLink()}, {"video", videoLink()}}},
                {"evaluate", "both"},
                {"coverage", {{"radiusM", 1500}, {"cellM", 250}, {"droneAglM", 80}}}};
    }
    /// A one-leg control-only mission from the pilot to `to`, opting into `modules`.
    static json shortMission(const json& to, const json& modules = json::object()) {
        return {{"schema", "mission/v1"},
                {"name", "Short"},
                {"time", {{"mode", "now"}}},
                {"pilot", point(48.50, 35.20, 1.5)},
                {"waypoints", {to}},
                {"returnToPilot", false},
                {"links", {{"control", controlLink()}}},
                {"evaluate", "control"},
                {"modules", modules},
                {"coverage", {{"radiusM", 1000}, {"cellM", 250}, {"droneAglM", 80}}}};
    }
    /// The edit session of a newly created copy of `m`.
    static std::string sessionFor(const json& m) {
        const auto r = call("POST", "/missions", m);
        if (r.status != 201) throw std::runtime_error("mission refused: " + r.body.dump());
        return r.body["session"];
    }
    /// Polls a coverage job until it leaves the queue.
    static json waitForJob(const std::string& id, const std::string& session) {
        json job;
        for (int i = 0; i < 600; ++i) {
            job = call("GET", "/jobs/" + id, nullptr, bearer(session)).body;
            if (job["state"] != "queued" && job["state"] != "running") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return job;
    }
    /// Whether `list` holds an object with `key` equal to `value`.
    static bool has(const json& list, const std::string& key, const json& value) {
        return std::ranges::any_of(list, [&](const json& j) { return j.is_object() && j.value(key, json()) == value; });
    }
};

}  // namespace

TEST_F(Integration, HealthAndSeededLibrary) {
    const auto h = call("GET", "/health");
    ASSERT_EQ(h.status, 200);
    EXPECT_TRUE(h.body["dem"].get<bool>());
    const auto map = call("GET", "/config/map");
    ASSERT_EQ(map.status, 200);
    EXPECT_EQ(map.body["basemap"]["pmtiles"], "/tiles/basemap.pmtiles");
    ASSERT_EQ(map.body["center"].size(), 2u);  // [lon, lat] of the DEM centre
    EXPECT_GT(map.body["center"][0].get<double>(), 35.0);
    EXPECT_GT(map.body["center"][1].get<double>(), 48.0);
    const auto lib = call("GET", "/library/radio-profiles");
    ASSERT_EQ(lib.status, 200) << lib.body;
    EXPECT_EQ(lib.body.size(), 14u);
    for (const auto& item : lib.body) EXPECT_FALSE(item["verified"].get<bool>()) << item["id"];
    for (const auto& [kind, count] :
         {std::pair{"antennas", 38u}, std::pair{"batteries", 21u}, std::pair{"drones", 3u}}) {
        const auto seeded = call("GET", std::string("/library/") + kind);
        ASSERT_EQ(seeded.status, 200) << seeded.body;
        EXPECT_EQ(seeded.body.size(), count) << kind;
        for (const auto& item : seeded.body) EXPECT_FALSE(item["verified"].get<bool>()) << kind << "/" << item["id"];
    }
}

TEST_F(Integration, MapConfigOffersOnlyBuiltLayers) {
    // Without a manifest every layer is offered.
    EXPECT_TRUE(call("GET", "/config/map").body.contains("satellite"));

    // A tiles-builder run without satellite imagery and without contours.
    const fs::path manifest = env->dir / "manifest.json";
    {
        std::ofstream out(manifest);
        out << json{{"files", json::array({{{"file", "basemap.pmtiles"}, {"attribution", "test basemap credit"}},
                                           {{"file", "hillshade.pmtiles"}},
                                           {{"file", "dem.cog.tif"}}})}}.dump();
    }
    const auto map = call("GET", "/config/map");
    fs::remove(manifest);
    ASSERT_EQ(map.status, 200);
    EXPECT_EQ(map.body["basemap"]["pmtiles"], "/tiles/basemap.pmtiles");
    EXPECT_EQ(map.body["hillshade"]["pmtiles"], "/tiles/hillshade.pmtiles");
    EXPECT_FALSE(map.body.contains("satellite")) << map.body.dump();
    EXPECT_FALSE(map.body.contains("contours")) << map.body.dump();
    EXPECT_EQ(map.body["attribution"], json::array({"test basemap credit"}));

    // A manifest the server cannot read is treated as none.
    {
        std::ofstream out(manifest);
        out << R"({"files": ["basemap.pmtiles", 2]})";
    }
    const auto unreadable = call("GET", "/config/map");
    fs::remove(manifest);
    ASSERT_EQ(unreadable.status, 200);
    EXPECT_TRUE(unreadable.body.contains("satellite"));
}

TEST_F(Integration, AdminAddsLibraryItemsWithVersions) {
    const json ground = {{"id", "test-ground-dipole"}, {"name", "Test ground dipole"}, {"version", 1}, {"verified", true},
                         {"type", "dipole"}, {"gainDbi", 2.15}, {"polarization", "linear-vertical"},
                         {"bandHz", {4e8, 6e9}}, {"feederLossDb", 0.5},
                         {"source", {{"kind", "standard"}, {"reference", "half-wave dipole, 2.15 dBi"}}}};
    json air = ground;
    air["id"] = "test-air-dipole";
    air["name"] = "Test air dipole";
    air.erase("feederLossDb");
    EXPECT_EQ(call("POST", "/library/antennas", ground).status, 401);  // not admin
    ASSERT_EQ(call("POST", "/library/antennas", ground, admin()).status, 201);
    ASSERT_EQ(call("POST", "/library/antennas", air, admin()).status, 201);
    EXPECT_EQ(call("POST", "/library/antennas", air, admin()).status, 409);
    json bad = air;
    bad["gainDbi"] = "high";
    const auto rejected = call("POST", "/library/antennas", bad, admin());
    EXPECT_EQ(rejected.status, 422);
    EXPECT_TRUE(rejected.body.contains("issues"));

    const json video = {{"id", "test-video-5g8"}, {"name", "Test digital video 5.8 GHz"}, {"version", 1},
                        {"verified", true}, {"kind", "video-digital"}, {"frequencyHz", 5.8e9},
                        {"txPowerDbm", {25, 30}}, {"latencyMs", {{"typical", 28}}},
                        {"sensitivity", {{"method", "computed"}, {"bandwidthHz", 2e7}, {"noiseFigureDb", 6},
                                         {"requiredSnrDb", 8}, {"source", {{"kind", "measurement"}, {"reference", "test bench"}}}}}};
    ASSERT_EQ(call("POST", "/library/radio-profiles", video, admin()).status, 201);

    // Edit: send the version you edited; the server stores the next one and keeps history.
    json edit = ground;
    edit["notes"] = "checked";
    const auto saved = call("PUT", "/library/antennas/test-ground-dipole", edit, admin());
    ASSERT_EQ(saved.status, 200) << saved.body;
    EXPECT_EQ(saved.body["version"], 2);
    EXPECT_EQ(call("PUT", "/library/antennas/test-ground-dipole", edit, admin()).status, 409);  // stale version
    const auto hist = call("GET", "/library/antennas/test-ground-dipole/history");
    ASSERT_EQ(hist.body.size(), 2u);
    EXPECT_EQ(hist.body[0]["version"], 1);
    EXPECT_EQ(hist.body[1]["item"]["notes"], "checked");
}

TEST_F(Integration, CreateMissionReturnsOwnerKeyOnce) {
    json bad = mission();
    bad.erase("pilot");
    EXPECT_EQ(call("POST", "/missions", bad).status, 422);
    const auto r = call("POST", "/missions", mission());
    ASSERT_EQ(r.status, 201) << r.body;
    env->missionId = r.body["missionId"];
    env->ownerKey = r.body["ownerKey"];
    env->editSession = r.body["session"];
    const auto g = call("GET", "/missions/" + env->missionId, nullptr, bearer(env->editSession));
    ASSERT_EQ(g.status, 200);
    EXPECT_EQ(g.body["role"], "edit");
    EXPECT_EQ(g.body["revision"], 1);
    EXPECT_FALSE(g.body.dump().find(env->ownerKey) != std::string::npos);
    EXPECT_EQ(call("GET", "/missions/" + env->missionId).status, 401);
}

TEST_F(Integration, ShareLinkWithPinAndReadOnlyRole) {
    EXPECT_EQ(call("POST", "/missions/" + env->missionId + "/links", {{"role", "view"}, {"pin", "123456"}}).status, 401);
    EXPECT_EQ(call("POST", "/missions/" + env->missionId + "/links", {{"role", "view"}, {"pin", "123456"}},
                   {{"X-Owner-Key", "rfo_guess"}}).status, 403);
    EXPECT_EQ(call("POST", "/missions/" + env->missionId + "/links", {{"role", "view"}, {"pin", "12345"}}, owner()).status,
              422);
    const auto link = call("POST", "/missions/" + env->missionId + "/links",
                           {{"role", "view"}, {"pin", "123456"}, {"label", "spotter"}}, owner());
    ASSERT_EQ(link.status, 201) << link.body;
    env->linkId = link.body["linkId"];
    env->linkToken = link.body["token"];
    const auto listed = call("GET", "/missions/" + env->missionId + "/links", nullptr, owner());
    ASSERT_EQ(listed.body.size(), 1u);
    EXPECT_FALSE(listed.body[0].contains("token"));

    // Before the PIN, the PIN screen may learn the role and the mission name, nothing else.
    const auto peek = call("GET", "/links/" + env->linkToken);
    ASSERT_EQ(peek.status, 200) << peek.body;
    EXPECT_EQ(peek.body, json({{"role", "view"}, {"missionName", "Ridge test"}}));
    EXPECT_EQ(call("GET", "/links/rfl_unknown").status, 404);

    const auto wrong = call("POST", "/links/" + env->linkToken + "/open", {{"pin", "000000"}});
    EXPECT_EQ(wrong.status, 401);
    EXPECT_EQ(wrong.body["triesLeft"], 4);
    const auto opened = call("POST", "/links/" + env->linkToken + "/open", {{"pin", "123456"}});
    ASSERT_EQ(opened.status, 200) << opened.body;
    EXPECT_EQ(opened.body["role"], "view");
    EXPECT_EQ(opened.body["missionName"], "Ridge test");
    const auto relisted = call("GET", "/missions/" + env->missionId + "/links", nullptr, owner());
    EXPECT_EQ(relisted.body[0]["openCount"], 1);
    env->viewSession = opened.body["session"];

    const auto g = call("GET", "/missions/" + env->missionId, nullptr, bearer(env->viewSession));
    ASSERT_EQ(g.status, 200);
    EXPECT_EQ(g.body["role"], "view");
    httplib::Headers h = bearer(env->viewSession);
    h.emplace("If-Match", "\"1\"");
    EXPECT_EQ(call("PUT", "/missions/" + env->missionId, mission(), h).status, 403);  // read-only link

    httplib::Headers e = bearer(env->editSession);
    e.emplace("If-Match", "\"1\"");
    json renamed = mission();
    renamed["name"] = "Ridge test 2";
    const auto saved = call("PUT", "/missions/" + env->missionId, renamed, e);
    ASSERT_EQ(saved.status, 200) << saved.body;
    EXPECT_EQ(saved.body["revision"], 2);
    EXPECT_EQ(call("PUT", "/missions/" + env->missionId, renamed, e).status, 409);  // revision 1 is stale
}

TEST_F(Integration, CalcPathIsExactAndHonest) {
    const json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.50, 35.30, 300)},
                      {"kind", "control"}, {"link", controlLink()}};
    EXPECT_EQ(call("POST", "/calc/path", req).status, 401);
    const auto r = call("POST", "/calc/path", req, bearer(env->viewSession));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.body["forecastTier"], "off");  // the mission does not opt into rf.weather
    const json& b = r.body;
    dump("calc-path", b);
    // Heights: AGL in, MSL from the DEM (flat 150 m at both ends).
    EXPECT_NEAR(b["ends"]["ground"]["groundMslM"].get<double>(), 150.0, 1e-3);
    EXPECT_NEAR(b["ends"]["air"]["antennaMslM"].get<double>(), 150.0 + 300.0, 1e-3);
    EXPECT_EQ(b["ends"]["transmitter"], "ground");
    // Free-space loss is exactly 20 log10(4 pi d f / c) over the ray.
    double fs = NAN;
    for (const auto& t : b["terms"]) {
        if (t["id"] == "free_space") fs = t["valueDb"];
    }
    const double d = b["rayLengthM"], f = b["frequencyHz"];
    EXPECT_NEAR(fs, 20.0 * std::log10(4.0 * rfcore::constants::kPi * d * f / rfcore::constants::kSpeedOfLight), 1e-9);
    // Received = tx - sum(terms); margin = received - (sensitivity + required margin).
    double sum = 0.0;
    for (const auto& t : b["terms"]) sum += t["valueDb"].get<double>();
    EXPECT_NEAR(b["receivedDbm"].get<double>(), 27.0 - sum, 1e-9);
    EXPECT_NEAR(b["marginDb"].get<double>(), b["receivedDbm"].get<double>() - (-120.0 + 10.0), 1e-9);
    // The uncertainty band and the notes carry the DEM error and the datasheet.
    ASSERT_TRUE(b.contains("band"));
    EXPECT_LE(b["band"]["worstDb"].get<double>(), b["marginDb"].get<double>());
    EXPECT_GE(b["band"]["bestDb"].get<double>(), b["marginDb"].get<double>());
    const std::string notes = b["uncertainty"].dump();
    EXPECT_NE(notes.find("synthetic test terrain"), std::string::npos);
    EXPECT_NE(notes.find("datasheet figure -120 dBm"), std::string::npos);
    EXPECT_NE(notes.find("not verified"), std::string::npos);  // seeded profile
    EXPECT_NE(notes.find("k = 4/3"), std::string::npos);      // no weather opted in
    // The library versions used are recorded.
    bool groundV2 = false;
    for (const auto& l : b["library"]) groundV2 |= l["id"] == "test-ground-dipole" && l["version"] == 2;
    EXPECT_TRUE(groundV2);
    EXPECT_EQ(b["profile"]["source"], "Synthetic ridge 3\"");
    EXPECT_EQ(b["inputHash"].get<std::string>().size(), 64u);

    // Behind the ridge at low altitude: the ridge governs the diffraction.
    json low = req;
    low["to"] = point(48.50, 35.30, 20);
    const auto blocked = call("POST", "/calc/path", low, bearer(env->viewSession));
    ASSERT_EQ(blocked.status, 200);
    EXPECT_FALSE(blocked.body["diffraction"]["lineOfSight"].get<bool>());
    EXPECT_NEAR(blocked.body["diffraction"]["dominant"]["lon"].get<double>(), 35.25, 0.002);
    EXPECT_GT(blocked.body["diffraction"]["lossDb"].get<double>(), 10.0);
}

TEST_F(Integration, MissingTerrainIsReportedNotGuessed) {
    const json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.42, 35.34, 100)},
                      {"kind", "control"}, {"link", controlLink()}};
    const auto r = call("POST", "/calc/path", req, bearer(env->viewSession));
    ASSERT_EQ(r.status, 422) << r.body;
    dump("calc-path-missing", r.body);
    EXPECT_EQ(r.body["status"], "missing-data");
    ASSERT_FALSE(r.body["missing"].empty());
    EXPECT_GT(r.body["missing"][0]["toM"].get<double>(), r.body["missing"][0]["fromM"].get<double>());
    // The box of the DEM samples without data, for the map to outline: [w, s, e, n].
    const json box = r.body["missingData"]["bbox"];
    ASSERT_EQ(box.size(), 4u) << r.body;
    EXPECT_LE(box[0].get<double>(), box[2].get<double>());
    EXPECT_LE(box[1].get<double>(), box[3].get<double>());
    EXPECT_GE(box[2].get<double>(), 35.20);
    EXPECT_LE(box[2].get<double>(), 35.34 + 1e-9);
    EXPECT_FALSE(r.body["missingData"]["source"].get<std::string>().empty());
}

TEST_F(Integration, BothLinksAndWeather) {
    json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.51, 35.24, 150)},
                {"links", {{"control", controlLink()}, {"video", videoLink()}}}, {"evaluate", "both"},
                {"modules", {{"rf.weather", json::object()}}}};
    const auto r = call("POST", "/calc/path", req, bearer(env->editSession));
    ASSERT_EQ(r.status, 200) << r.body;
    dump("calc-path-both-weather", r.body);
    ASSERT_TRUE(r.body.contains("control"));
    ASSERT_TRUE(r.body.contains("video"));
    EXPECT_EQ(r.body["video"]["ends"]["transmitter"], "air");
    EXPECT_EQ(r.body["video"]["weather"]["tier"], "hourly");
    EXPECT_EQ(r.body["video"]["forecastTier"], "hourly");
    // k comes from the forecast gradient, not 4/3.
    EXPECT_NE(r.body["video"]["earth"]["effectiveEarthFactor"].get<double>(), 4.0 / 3.0);
    EXPECT_NE(r.body["video"]["earth"]["source"].get<std::string>().find("P.453"), std::string::npos);
    // Rain at 5.8 GHz is a term; at 915 MHz P.838 does not apply and the result says so.
    bool rain = false;
    for (const auto& t : r.body["video"]["terms"]) rain |= t["id"] == "rf.weather/rain";
    EXPECT_TRUE(rain);
    EXPECT_NE(r.body["control"]["uncertainty"].dump().find("1-1000 GHz"), std::string::npos);
    EXPECT_NE(env->weather->lastQuery.find("geopotential_height_850hPa"), std::string::npos);

    // Beyond the forecast: said so, standard k, no rain.
    req["timeUtc"] = isoUtc(Clock::now() + std::chrono::hours(24 * 30));
    const auto far = call("POST", "/calc/path", req, bearer(env->editSession));
    ASSERT_EQ(far.status, 200);
    EXPECT_EQ(far.body["video"]["weather"]["tier"], "none");
    EXPECT_NE(far.body["video"]["uncertainty"].dump().find("outside the forecast"), std::string::npos);
    EXPECT_DOUBLE_EQ(far.body["video"]["earth"]["effectiveEarthFactor"].get<double>(), 4.0 / 3.0);
}

TEST_F(Integration, CalcRouteWorstSamplePerLinkPerLeg) {
    json m = mission();
    m["name"] = "Ridge test 2";
    const auto r = call("POST", "/calc/route", m, bearer(env->editSession));
    ASSERT_EQ(r.status, 200) << r.body;
    dump("calc-route", r.body);
    const json& legs = r.body["legs"];
    ASSERT_EQ(legs.size(), 3u);  // pilot -> wp1 -> wp2 -> pilot
    EXPECT_EQ(legs[0]["from"], "Pilot");
    EXPECT_EQ(legs[2]["to"], "Pilot");
    for (const auto& leg : legs) {
        ASSERT_TRUE(leg.contains("worst")) << leg.dump();
        EXPECT_TRUE(leg["worst"].contains("control"));
        EXPECT_TRUE(leg["worst"].contains("video"));
        EXPECT_TRUE(leg["worst"]["control"].contains("band"));
        EXPECT_GT(leg["samples"]["evaluated"].get<int>(), 10);
        const std::string status = leg["status"];
        EXPECT_TRUE(status == "ok" || status == "warn" || status == "blocked") << status;
        EXPECT_FALSE(leg["reasons"].empty());
    }
    // Leg 1 ends behind the ridge at 60 m AGL: its worst control sample is there or near it.
    EXPECT_GT(legs[1]["worst"]["control"]["sample"]["alongLegM"].get<double>(), 0.0);
    EXPECT_EQ(r.body["missionRevision"], 2);  // computed for the saved document
    EXPECT_TRUE(r.body["moduleFailures"].empty());

    // The result was stored as the mission's last result.
    const auto g = call("GET", "/missions/" + env->missionId, nullptr, bearer(env->viewSession));
    ASSERT_TRUE(g.body.contains("lastResult"));
    EXPECT_EQ(g.body["lastResult"]["inputHash"], r.body["inputHash"]);

    // Same inputs, same hash: served from the result cache.
    json planned = m;
    planned["time"] = {{"mode", "planned"}, {"utc", isoUtc(Clock::now())}};
    const auto first = call("POST", "/calc/route", planned, bearer(env->editSession));
    ASSERT_EQ(first.status, 200) << first.body;
    const auto again = call("POST", "/calc/route", planned, bearer(env->editSession));
    EXPECT_EQ(again.body["inputHash"], first.body["inputHash"]);
    const auto tiers = call("GET", "/admin/cache", nullptr, admin());
    ASSERT_EQ(tiers.status, 200);
    EXPECT_GT(tiers.body[1]["hits"].get<int>(), 0);
}

TEST_F(Integration, ThirdPartyModuleAddsItsTermAndForeignBundlesStayPrivate) {
    // Admin trusts the third-party test key and uploads the example module packed with it.
    const std::string pub = readFile(RFMOD_TEST_KEYS "/dev-thirdparty.pub");
    EXPECT_EQ(call("POST", "/admin/keys", {{"key", pub}, {"label", "test vendor"}}).status, 401);
    ASSERT_EQ(call("POST", "/admin/keys", {{"key", pub}, {"label", "test vendor"}}, admin()).status, 201);
    const auto keys = call("GET", "/admin/keys", nullptr, admin());
    ASSERT_EQ(keys.body.size(), 2u);
    EXPECT_EQ(call("DELETE", "/admin/keys/" + keys.body[0]["id"].get<std::string>(), nullptr, admin()).status, 409);

    const std::string pkg = rfmod::PackageWriter::pack(
        fs::path(BODY_SHADOW_STAGE), rfmod::SecretKey::fromText(readFile(RFMOD_TEST_KEYS "/dev-thirdparty.secret")));
    httplib::Client c("127.0.0.1", env->port);
    c.set_read_timeout(60, 0);
    auto up = c.Post("/api/v1/admin/modules?install=true", admin(), pkg, "application/octet-stream");
    ASSERT_TRUE(up);
    ASSERT_EQ(up->status, 200) << up->body;
    const json report = json::parse(up->body);
    ASSERT_TRUE(report["ok"].get<bool>()) << report.dump(2);
    dump("upload-report", report);

    const auto opts = call("PUT", "/admin/modules/example.body-shadow",
                           {{"options", {{"lossDb", 3.5}, {"source", "test measurement"}, {"uncertaintyDb", 1.0}}}}, admin());
    ASSERT_EQ(opts.status, 200) << opts.body;

    const auto mods = call("GET", "/modules");
    std::set<std::string> ids;
    for (const auto& m : mods.body) ids.insert(m["id"]);
    for (const char* id : {"rf.weather", "rf.clutter", "rf.uncertainty", "rf.radio-profiles", "example.body-shadow"}) {
        EXPECT_TRUE(ids.count(id)) << id;
    }

    json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.50, 35.30, 300)},
                {"kind", "control"}, {"link", controlLink()}};
    const auto base = call("POST", "/calc/path", req, bearer(env->viewSession));
    req["modules"] = {{"example.body-shadow", json::object()}};
    const auto with = call("POST", "/calc/path", req, bearer(env->viewSession));
    ASSERT_EQ(with.status, 200) << with.body;
    bool term = false;
    for (const auto& t : with.body["terms"]) term |= t["id"] == "example.body-shadow/ground" && t["valueDb"] == 3.5;
    EXPECT_TRUE(term) << with.body["terms"].dump();
    EXPECT_NEAR(base.body["marginDb"].get<double>() - with.body["marginDb"].get<double>(), 3.5, 1e-9);
    EXPECT_TRUE(with.body["moduleFailures"].empty());
    EXPECT_NE(with.body["uncertainty"].dump().find("Body shadow"), std::string::npos);

    // lib/ is never served, whatever the path.
    EXPECT_EQ(call("GET", "/modules/example.body-shadow/lib/libbody_shadow.so").status, 404);
    EXPECT_EQ(call("GET", "/modules/example.body-shadow/schema/options.json").status, 200);

    // Disabling a built-in takes it out of calculations.
    ASSERT_EQ(call("PUT", "/admin/modules/rf.uncertainty", {{"enabled", false}}, admin()).status, 200);
    req.erase("modules");
    req["to"] = point(48.50, 35.30, 310);
    const auto noBand = call("POST", "/calc/path", req, bearer(env->viewSession));
    EXPECT_FALSE(noBand.body.contains("band"));
    ASSERT_EQ(call("PUT", "/admin/modules/rf.uncertainty", {{"enabled", true}}, admin()).status, 200);
}

TEST_F(Integration, CoverageJob) {
    const auto start = call("POST", "/calc/coverage", mission(), bearer(env->viewSession));
    ASSERT_EQ(start.status, 202) << start.body;
    const std::string id = start.body["jobId"];
    json job;
    for (int i = 0; i < 600; ++i) {
        job = call("GET", "/jobs/" + id, nullptr, bearer(env->viewSession)).body;
        if (job["state"] != "queued" && job["state"] != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(job["state"], "done") << job.dump();
    for (const auto& part : job["parts"]) EXPECT_TRUE(part["link"] == "control" || part["link"] == "video") << part;
    const auto grid = call("GET", "/jobs/" + id + "/coverage/control.json", nullptr, bearer(env->viewSession));
    ASSERT_EQ(grid.status, 200) << grid.body;
    EXPECT_EQ(grid.body["cols"], 13);
    EXPECT_EQ(grid.body["rows"], 13);
    EXPECT_EQ(grid.body["marginDb"].size(), 169u);
    EXPECT_TRUE(grid.body["marginDb"][6 * 13 + 6].is_null());  // the pilot's own cell
    EXPECT_FALSE(grid.body["marginDb"][6 * 13 + 7].is_null());
    EXPECT_EQ(grid.body["limitPolygon"]["type"], "MultiPolygon");
    EXPECT_FALSE(grid.body["limitPolygon"]["coordinates"].empty());
    // Another mission's session cannot see the job.
    const auto other = call("POST", "/missions", mission());
    EXPECT_EQ(call("GET", "/jobs/" + id, nullptr, bearer(other.body["session"])).status, 404);
}

TEST_F(Integration, RevokeAndLockout) {
    EXPECT_EQ(call("DELETE", "/missions/" + env->missionId + "/links/" + env->linkId, nullptr, bearer(env->editSession)).status,
              401);  // a session is not the owner key
    ASSERT_EQ(call("DELETE", "/missions/" + env->missionId + "/links/" + env->linkId, nullptr, owner()).status, 204);
    EXPECT_EQ(call("GET", "/missions/" + env->missionId, nullptr, bearer(env->viewSession)).status, 401);
    EXPECT_EQ(call("POST", "/links/" + env->linkToken + "/open", {{"pin", "123456"}}).status, 410);

    const auto link = call("POST", "/missions/" + env->missionId + "/links", {{"role", "edit"}, {"pin", "654321"}}, owner());
    const std::string token = link.body["token"];
    for (int i = 0; i < 4; ++i) EXPECT_EQ(call("POST", "/links/" + token + "/open", {{"pin", "111111"}}).status, 401);
    const auto locked = call("POST", "/links/" + token + "/open", {{"pin", "111111"}});
    EXPECT_EQ(locked.status, 429);
    EXPECT_EQ(locked.headers.find("Retry-After")->second, "900");
    EXPECT_EQ(call("POST", "/links/" + token + "/open", {{"pin", "654321"}}).status, 429);
}

TEST_F(Integration, AdminCacheLimitsAndClear) {
    EXPECT_EQ(call("GET", "/admin/cache", nullptr, bearer("wrong")).status, 401);
    const auto put = call("PUT", "/admin/cache",
                          json::array({{{"id", "dem-blocks"}, {"limitBytes", 1 << 20}},
                                       {{"id", "results-table"}, {"limitRows", 1}, {"limitBytes", 1 << 30}}}),
                          admin());
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_EQ(put.body[0]["limitBytes"], 1 << 20);
    EXPECT_LE(put.body[0]["usedBytes"].get<long long>(), 1 << 20);
    EXPECT_LE(put.body[2]["entries"].get<int>(), 1);
    ASSERT_EQ(call("DELETE", "/admin/cache", nullptr, admin()).status, 204);
    const auto after = call("GET", "/admin/cache", nullptr, admin());
    EXPECT_EQ(after.body[1]["entries"], 0);
    EXPECT_EQ(after.body[2]["entries"], 0);
}

TEST_F(Integration, ModuleAnswersAreCheckedAndFailuresReported) {
    const std::string pkg = rfmod::PackageWriter::pack(
        fs::path(PROBE_STAGE), rfmod::SecretKey::fromText(readFile(RFMOD_TEST_KEYS "/dev-owner.secret")));
    httplib::Client c("127.0.0.1", env->port);
    c.set_read_timeout(60, 0);
    const auto up = c.Post("/api/v1/admin/modules?install=true", admin(), pkg, "application/octet-stream");
    ASSERT_TRUE(up);
    ASSERT_EQ(up->status, 200) << up->body;
    ASSERT_TRUE(json::parse(up->body)["ok"].get<bool>()) << up->body;

    // Good reasons land on their leg or the route, an unknown severity becomes info, and the
    // module's data and battery estimate are kept.
    const json answer = {{"test.probe", {{"mode", "answer"}}}};
    const json m = shortMission(point(48.51, 35.22, 100), answer);
    const auto r = call("POST", "/calc/route", m, bearer(sessionFor(m)));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(has(r.body["legs"][0]["reasons"], "text", "on the first leg"));
    EXPECT_TRUE(has(r.body["reasons"], "text", "on a leg that does not exist"));
    EXPECT_TRUE(has(r.body["reasons"], "text", "leg is not a number"));
    for (const auto& reason : r.body["reasons"]) {
        if (reason.value("text", "") == "unknown severity") EXPECT_EQ(reason["severity"], "info");
    }
    EXPECT_EQ(r.body["modules"]["test.probe"], json({{"probe", true}}));
    EXPECT_EQ(r.body["battery"]["source"], "test.probe");
    EXPECT_TRUE(r.body["moduleFailures"].empty()) << r.body["moduleFailures"];

    // A module that fails, or answers something that is not an object, is listed once, and the
    // route is still computed without it.
    for (const char* mode : {"fail", "not-an-object"}) {
        const json bad = shortMission(point(48.51, 35.22, 100), {{"test.probe", {{"mode", mode}}}});
        const auto failed = call("POST", "/calc/route", bad, bearer(sessionFor(bad)));
        ASSERT_EQ(failed.status, 200) << failed.body;
        EXPECT_EQ(failed.body["legs"].size(), 1u);
        const json& failures = failed.body["moduleFailures"];
        EXPECT_EQ(std::ranges::count_if(failures, [](const json& f) { return f["module"] == "test.probe"; }), 1)
            << mode << ": " << failures;
        EXPECT_FALSE(failed.body.contains("battery")) << mode;
    }
    const auto notObject = call("POST", "/calc/route", shortMission(point(48.51, 35.22, 100), {{"test.probe", {{"mode", "not-an-object"}}}}),
                                bearer(sessionFor(m)));
    EXPECT_NE(notObject.body["moduleFailures"].dump().find("did not return an object"), std::string::npos);

    // A failing loss contributor on a single path.
    const json path = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.50, 35.30, 300)}, {"kind", "control"},
                       {"link", controlLink()}, {"modules", {{"test.probe", {{"mode", "fail"}}}}}};
    const auto p = call("POST", "/calc/path", path, bearer(sessionFor(m)));
    ASSERT_EQ(p.status, 200) << p.body;
    EXPECT_TRUE(has(p.body["moduleFailures"], "module", "test.probe")) << p.body["moduleFailures"];

    // Coverage: only the layer with one value per cell is kept; a failing module is reported.
    const std::string session = sessionFor(m);
    const auto start = call("POST", "/calc/coverage", m, bearer(session));
    ASSERT_EQ(start.status, 202) << start.body;
    ASSERT_EQ(waitForJob(start.body["jobId"], session)["state"], "done");
    const auto grid = call("GET", "/jobs/" + start.body["jobId"].get<std::string>() + "/coverage/control.json", nullptr,
                           bearer(session));
    ASSERT_EQ(grid.status, 200) << grid.body;
    const json& layers = grid.body["modules"]["test.probe"]["layers"];
    ASSERT_EQ(layers.size(), 1u) << grid.body["modules"];
    EXPECT_EQ(layers[0]["id"], "fits");
    EXPECT_TRUE(has(grid.body["reasons"], "text", "probe looked at the grid"));

    const json failing = shortMission(point(48.51, 35.22, 100), {{"test.probe", {{"mode", "fail"}}}});
    const std::string failingSession = sessionFor(failing);
    const auto failingStart = call("POST", "/calc/coverage", failing, bearer(failingSession));
    ASSERT_EQ(failingStart.status, 202);
    ASSERT_EQ(waitForJob(failingStart.body["jobId"], failingSession)["state"], "done");
    const auto failingGrid = call("GET", "/jobs/" + failingStart.body["jobId"].get<std::string>() + "/coverage/control.json",
                                  nullptr, bearer(failingSession));
    EXPECT_TRUE(has(failingGrid.body["moduleFailures"], "module", "test.probe")) << failingGrid.body;

    // Removal: built-ins can only be disabled; an uploaded module goes away.
    EXPECT_EQ(call("GET", "/admin/modules/notices", nullptr, admin()).status, 200);
    EXPECT_EQ(call("GET", "/admin/modules/notices").status, 401);
    EXPECT_EQ(call("DELETE", "/admin/modules/rf.weather", nullptr, admin()).status, 422);
    const auto removed = call("DELETE", "/admin/modules/test.probe", nullptr, admin());
    EXPECT_LT(removed.status, 300) << removed.body;
    EXPECT_FALSE(has(call("GET", "/modules").body, "id", "test.probe"));
}

TEST_F(Integration, RouteIntoTheHoleIsMissingDataNotAVerdict) {
    const json m = shortMission(point(48.42, 35.36, 100));
    const auto r = call("POST", "/calc/route", m, bearer(sessionFor(m)));
    ASSERT_EQ(r.status, 200) << r.body;
    const json& leg = r.body["legs"][0];
    EXPECT_EQ(leg["status"], "missing-data") << leg;
    EXPECT_GT(leg["samples"]["missingData"].get<int>(), 0);
    ASSERT_FALSE(leg["missing"].empty());
    EXPECT_LE(leg["missing"].size(), 20u);
    EXPECT_TRUE(leg["missing"][0].contains("alongLegM"));
    EXPECT_EQ(leg["missingData"]["bbox"].size(), 4u);
    EXPECT_EQ(leg["reasons"][0]["severity"], "blocked");
}

TEST_F(Integration, CoverageCountsCellsWithoutTerrainAndHonoursLimits) {
    json m = shortMission(point(48.46, 35.32, 100));
    m["pilot"] = point(48.455, 35.325, 1.5);
    m["coverage"] = {{"radiusM", 1500}, {"cellM", 250}, {"droneAglM", 80}};
    const std::string session = sessionFor(m);
    const auto start = call("POST", "/calc/coverage", m, bearer(session));
    ASSERT_EQ(start.status, 202) << start.body;
    ASSERT_EQ(waitForJob(start.body["jobId"], session)["state"], "done");
    const auto grid =
        call("GET", "/jobs/" + start.body["jobId"].get<std::string>() + "/coverage/control.json", nullptr, bearer(session));
    ASSERT_EQ(grid.status, 200) << grid.body;
    EXPECT_GT(grid.body["missingDataCells"].get<int>(), 0);

    // More cells than the server allows.
    json huge = m;
    huge["coverage"] = {{"radiusM", 20000}, {"cellM", 50}, {"droneAglM", 80}};
    EXPECT_EQ(call("POST", "/calc/coverage", huge, bearer(session)).status, 422);

    // A job can be cancelled. Cancelling only raises a flag the runner checks between rows, so a
    // job that already finished stays done.
    json big = m;
    big["pilot"] = point(48.52, 35.25, 1.5);
    big["coverage"] = {{"radiusM", 6000}, {"cellM", 100}, {"droneAglM", 80}};
    const std::string bigSession = sessionFor(big);
    const auto bigStart = call("POST", "/calc/coverage", big, bearer(bigSession));
    ASSERT_EQ(bigStart.status, 202) << bigStart.body;
    const std::string id = bigStart.body["jobId"];
    EXPECT_EQ(call("DELETE", "/jobs/" + id, nullptr, bearer(bigSession)).status, 204);
    const auto state = waitForJob(id, bigSession)["state"];
    EXPECT_TRUE(state == "cancelled" || state == "done") << state;
}

TEST_F(Integration, DataEndpointsAndRequestErrors) {
    const auto profile = call("GET", "/elevation/profile?from=48.5,35.2&to=48.5,35.3");
    ASSERT_EQ(profile.status, 200) << profile.body;
    EXPECT_GT(profile.body.at("points").size(), 10u);
    const auto holed = call("GET", "/elevation/profile?from=48.5,35.2&to=48.42,35.36");
    EXPECT_EQ(holed.status, 422);
    EXPECT_EQ(holed.body["status"], "missing-data") << holed.body;
    EXPECT_EQ(call("GET", "/elevation/profile?from=48.5,35.2").status, 400);

    const auto item = call("GET", "/library/radio-profiles/elrs-915-50hz");
    ASSERT_EQ(item.status, 200);
    EXPECT_EQ(item.body["id"], "elrs-915-50hz");
    EXPECT_EQ(call("GET", "/library/antennas/no-such-antenna").status, 404);

    const auto weather = call("GET", "/weather/point?lat=48.5&lon=35.2");
    ASSERT_EQ(weather.status, 200) << weather.body;
    EXPECT_TRUE(weather.body["notes"].is_array());
    EXPECT_EQ(call("GET", "/weather/point?lat=48.5&lon=35.2&timeUtc=soon").status, 400);
    EXPECT_EQ(call("GET", "/weather/point?lat=91&lon=35.2").status, 400);
    const auto horizon = call("GET", "/weather/horizon");
    ASSERT_EQ(horizon.status, 200) << horizon.body;
    EXPECT_TRUE(horizon.body.contains("hourlyUntilUtc")) << horizon.body;

    // A body that is not JSON, and a save without the revision it edits.
    httplib::Client c("127.0.0.1", env->port);
    const auto garbled = c.Post("/api/v1/missions", "{not json", "application/json");
    ASSERT_TRUE(garbled);
    EXPECT_EQ(garbled->status, 400);
    const auto created = call("POST", "/missions", mission());
    const std::string id = created.body["missionId"];
    const httplib::Headers edit = bearer(created.body["session"]);
    EXPECT_EQ(call("PUT", "/missions/" + id, mission(), edit).status, 428);
    httplib::Headers wrongTag = edit;
    wrongTag.emplace("If-Match", "\"seven\"");
    EXPECT_EQ(call("PUT", "/missions/" + id, mission(), wrongTag).status, 428);

    // Share links with an expiry.
    const httplib::Headers ownerKey = {{"X-Owner-Key", created.body["ownerKey"]}};
    const auto expiring = call("POST", "/missions/" + id + "/links",
                               {{"role", "view"}, {"pin", "246810"}, {"expiresAt", isoUtc(Clock::now() + std::chrono::hours(2))}},
                               ownerKey);
    ASSERT_EQ(expiring.status, 201) << expiring.body;
    EXPECT_TRUE(expiring.body.contains("expiresAt"));
    EXPECT_EQ(call("POST", "/missions/" + id + "/links", {{"role", "view"}, {"pin", "246810"}, {"expiresAt", "next week"}},
                   ownerKey).status,
              422);
}

TEST_F(Integration, ClutterRaisesTheProfileWhereTheAdminSaysSo) {
    const json classes = {{"10", {{"enabled", true}, {"heightM", 20}, {"source", "test survey"}}},
                          {"50", {{"enabled", true}, {"heightM", 10}, {"source", "test survey"}}}};
    ASSERT_EQ(call("PUT", "/admin/modules/rf.clutter", {{"options", {{"classes", classes}}}}, admin()).status, 200);
    json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.50, 35.24, 30)}, {"kind", "control"},
                {"link", controlLink()}};
    const std::string session = sessionFor(mission());
    const auto bare = call("POST", "/calc/path", req, bearer(session));
    ASSERT_EQ(bare.status, 200) << bare.body;

    req["modules"] = {{"rf.clutter", {{"disabledClasses", {50, "x"}}}}};
    const auto trees = call("POST", "/calc/path", req, bearer(session));
    ASSERT_EQ(trees.status, 200) << trees.body;
    const std::string notes = trees.body["uncertainty"].dump();
    EXPECT_NE(notes.find("classes turned off for this mission: 50"), std::string::npos) << notes;
    EXPECT_LT(trees.body["marginDb"].get<double>(), bare.body["marginDb"].get<double>());
    bool tree = false;
    for (const auto& s : trees.body.at("profile").at("points")) tree |= s.value("landCover", 0) == 10;
    EXPECT_TRUE(tree);
    ASSERT_EQ(call("PUT", "/admin/modules/rf.clutter", {{"options", json::object()}}, admin()).status, 200);
}

TEST_F(Integration, AnalogVideoGetsAPictureGrade) {
    const json grades = {{{"label", "clear"}, {"requiredSnrDb", 10}},
                         {{"label", "noisy"}, {"requiredSnrDb", 4}},
                         {{"label", "lost"}, {"requiredSnrDb", 0}}};
    const json bench = {{"kind", "measurement"}, {"reference", "test bench"}};
    json computed = {{"id", "test-analog-5g8"}, {"name", "Test analog 5.8 GHz"}, {"version", 1}, {"verified", true},
                     {"kind", "video-analog"}, {"frequencyHz", 5.8e9}, {"txPowerDbm", {25}},
                     {"latencyMs", {{"typical", 1}, {"max", 2}}}, {"analogGrades", grades}, {"extraNoiseDb", 2},
                     {"sensitivity", {{"method", "computed"}, {"bandwidthHz", 8e6}, {"noiseFigureDb", 8},
                                      {"requiredSnrDb", 0}, {"source", bench}}}};
    ASSERT_EQ(call("POST", "/library/radio-profiles", computed, admin()).status, 201);
    json datasheet = computed;
    datasheet["id"] = "test-analog-datasheet";
    datasheet["sensitivity"] = {{"method", "datasheet"}, {"dBm", -90}, {"source", bench}};
    ASSERT_EQ(call("POST", "/library/radio-profiles", datasheet, admin()).status, 201);

    json link = videoLink();
    link["radio"] = "test-analog-5g8";
    const json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.51, 35.21, 100)}, {"kind", "video"}, {"link", link}};
    const std::string session = sessionFor(mission());
    const auto graded = call("POST", "/calc/path", req, bearer(session));
    ASSERT_EQ(graded.status, 200) << graded.body;
    EXPECT_TRUE(graded.body.contains("snrDb"));
    EXPECT_EQ(graded.body["analogGrade"], "clear") << graded.body["snrDb"];
    EXPECT_EQ(graded.body["latencyMaxMs"], 2);

    json far = req;
    far["to"] = point(48.50, 35.32, 5);
    EXPECT_NE(call("POST", "/calc/path", far, bearer(session)).body.value("analogGrade", ""), "clear");

    json fromSheet = req;
    fromSheet["link"]["radio"] = "test-analog-datasheet";
    const auto ungraded = call("POST", "/calc/path", fromSheet, bearer(session));
    ASSERT_EQ(ungraded.status, 200) << ungraded.body;
    EXPECT_FALSE(ungraded.body.contains("analogGrade"));
    EXPECT_NE(ungraded.body["uncertainty"].dump().find("Analog grade"), std::string::npos);
}

TEST_F(Integration, AlphaBundlesNeedTheAdminOrTheirGrant) {
    // The probe, restaged as an alpha module with a UI bundle.
    const fs::path stage = fs::temp_directory_path() / "rfmap-alpha-probe-stage";
    fs::remove_all(stage);
    fs::copy(fs::path(PROBE_STAGE), stage, fs::copy_options::recursive);
    json manifest = json::parse(readFile(stage / "manifest.json"));
    manifest["lifecycle"] = "alpha";
    manifest["provides"].push_back("ui.panel");
    manifest["ui"] = {{"bundle", "ui/panel.js"}, {"slots", {"side.panels"}}};
    std::ofstream(stage / "manifest.json") << manifest.dump(2);
    fs::create_directories(stage / "ui");
    std::ofstream(stage / "ui" / "panel.js") << "export default {};\n";
    const std::string pkg =
        rfmod::PackageWriter::pack(stage, rfmod::SecretKey::fromText(readFile(RFMOD_TEST_KEYS "/dev-owner.secret")));
    fs::remove_all(stage);
    httplib::Client c("127.0.0.1", env->port);
    c.set_read_timeout(60, 0);
    const auto up = c.Post("/api/v1/admin/modules?install=true", admin(), pkg, "application/octet-stream");
    ASSERT_TRUE(up);
    ASSERT_EQ(up->status, 200) << up->body;
    ASSERT_TRUE(json::parse(up->body)["ok"].get<bool>()) << up->body;

    const auto probeIn = [](const json& list) {
        const auto it = std::ranges::find_if(list, [](const json& m) { return m["id"] == "test.probe"; });
        return it == list.end() ? json() : *it;
    };
    // Anyone else sees the module without its bundle; the admin's listing signs the bundle URL.
    EXPECT_FALSE(probeIn(call("GET", "/modules").body).contains("ui"));
    const json listed = probeIn(call("GET", "/modules", nullptr, admin()).body);
    ASSERT_TRUE(listed.contains("ui")) << listed;
    const std::string bundle = listed["ui"]["bundle"].get<std::string>();
    const auto grant = bundle.find("?grant=");
    ASSERT_NE(grant, std::string::npos) << bundle;

    const std::string file = "/api/v1/modules/test.probe/ui/panel.js";
    const auto granted = c.Get(file + bundle.substr(grant));
    ASSERT_TRUE(granted);
    EXPECT_EQ(granted->status, 200);
    EXPECT_EQ(granted->get_header_value("Cache-Control"), "private, no-store");
    EXPECT_EQ(c.Get(file)->status, 404);
    EXPECT_EQ(c.Get(file + "?grant=1.forged")->status, 404);
    EXPECT_EQ(c.Get(file, admin())->status, 200);

    EXPECT_LT(call("DELETE", "/admin/modules/test.probe", nullptr, admin()).status, 300);
}

TEST_F(Integration, AntennasOutOfBandOrCrossPolarisedAreNoted) {
    const json patch = {{"id", "test-air-patch-5g8"}, {"name", "Test 5.8 GHz patch"}, {"version", 1}, {"verified", true},
                        {"type", "patch"}, {"gainDbi", 8}, {"polarization", "rhcp"},
                        {"bandHz", {5.6e9, 6e9}},
                        {"source", {{"kind", "standard"}, {"reference", "test patch"}}}};
    ASSERT_EQ(call("POST", "/library/antennas", patch, admin()).status, 201);
    json link = controlLink();
    link["airAntenna"] = "test-air-patch-5g8";
    const json req = {{"from", point(48.50, 35.20, 1.5)}, {"to", point(48.51, 35.21, 100)}, {"kind", "control"}, {"link", link}};
    const auto r = call("POST", "/calc/path", req, bearer(sessionFor(mission())));
    ASSERT_EQ(r.status, 200) << r.body;
    const std::string body = r.body.dump();
    EXPECT_NE(body.find("Test 5.8 GHz patch is specified for 5600-6000 MHz"), std::string::npos) << body;
    EXPECT_NE(body.find("differ in polarisation"), std::string::npos) << body;
}
