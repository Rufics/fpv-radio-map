// End to end: the server against a local PostGIS cluster (ctest fixture), a synthetic DEM
// COG written with GDAL, a stand-in Open-Meteo, and the example body-shadow module.

#include "app.hpp"
#include "test_support.hpp"

#include <rfcore/constants.hpp>
#include <rfmod/package.hpp>

#include <httplib.h>

#include <gtest/gtest.h>

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
            lastQuery = req.params.count("hourly") ? req.get_param_value("hourly") : "";
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
                      {"demName", "Synthetic ridge 3\""}}},
                    {"schemasDir", RFMAP_SCHEMAS_DIR},
                    {"seedDir", RFMAP_SEED_DIR},
                    {"modules",
                     {{"root", (env->dir / "modules").string()},
                      {"worker", RFMOD_WORKER},
                      {"trustStore", (env->dir / "keys.json").string()}}},
                    {"weather", {{"baseUrl", "http://127.0.0.1:" + std::to_string(env->weather->port)}}},
                    {"auth", {{"argon2Cost", "interactive"}, {"pinLockoutMinutes", 15}}},
                    {"coverage", {{"threads", 4}}},
                    // Generous limits: the test makes many requests from one address.
                    {"rateLimits", {{"default", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"calc", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"admin", {{"perMinute", 6000}, {"burst", 1000}}},
                                    {"open", {{"perMinute", 6000}, {"burst", 1000}}}}}};
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
                      httplib::Headers headers = {}) {
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
