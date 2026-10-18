// The host calls a module makes (docs/modules/developer-guide.md section 6), answered by
// ServerHostServices over the real services: the library in PostgreSQL, a synthetic DEM COG and a
// canned forecast. Linked into the integration binary, which ctest gives a throw-away database.

#include "host_services.hpp"
#include "logger.hpp"
#include "test_support.hpp"

#include <rfmod/abi.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

// Flat ground at 200 m with a NoData hole south-east of (48.52, 35.22). 3 arc-second pixels.
constexpr double kWest = 35.15, kNorth = 48.60, kPx = 1.0 / 1200.0;
constexpr int kCols = 120, kRows = 120;

double flatWithHole(int c, int r) {
    const double lon = kWest + (c + 0.5) * kPx;
    const double lat = kNorth - (r + 0.5) * kPx;
    return lat < 48.52 && lon > 35.22 ? NAN : 200.0;
}

/// Three hours of surface weather around the hour the rig was built in, 10 C in that hour.
class NowForecast final : public IForecastSource {
public:
    json fetch(double, double, const std::string&) override {
        ++fetches;
        const long long t0 = Clock::to_time_t(hour - std::chrono::hours(1));
        return {{"elevation", 200.0},
                {"hourly",
                 {{"time", {t0, t0 + 3600, t0 + 7200}},
                  {"temperature_2m", {9.0, 10.0, 11.0}},
                  {"relative_humidity_2m", {70.0, 70.0, 70.0}},
                  {"surface_pressure", {990.0, 990.0, 990.0}},
                  {"wind_speed_10m", {3.0, 3.0, 3.0}},
                  {"wind_direction_10m", {180.0, 180.0, 180.0}}}}};
    }
    std::optional<TimePoint> runTime() override { return std::nullopt; }
    const TimePoint hour = std::chrono::floor<std::chrono::hours>(Clock::now());
    int fetches = 0;
};

json point(double lat, double lon) { return {{"lat", lat}, {"lon", lon}}; }

json antenna(const std::string& id, double feederLossDb) {
    json a = {{"id", id}, {"name", id}, {"verified", true}, {"type", "dipole"}, {"gainDbi", 2.15},
              {"polarization", "linear-vertical"}, {"bandHz", {4e8, 6e9}},
              {"source", {{"kind", "standard"}, {"reference", "half-wave dipole, 2.15 dBi"}}}};
    if (feederLossDb > 0.0) a["feederLossDb"] = feederLossDb;
    return a;
}

/// The services a running server binds to its host calls, built on a fresh database.
class HostRig {
public:
    explicit HostRig(const std::string& url)
        : dir_(rfmap::test::scratchDir("host-calls")), db_(prepare(url), 1), schemas_(RFMAP_SCHEMAS_DIR),
          library_(db_, schemas_),
          blocks_(16u << 20, [](const RasterBlock& b) { return b.values.size() * sizeof(float) + sizeof(RasterBlock); }),
          weather_(weatherConfig_, &forecast_, clock_) {
        (void)db_.migrate(RFMAP_SQL_DIR);
        builtins_ = std::make_unique<BuiltinModules>(&db_, schemas_, "test");
        (void)library_.seed(RFMAP_SEED_DIR);
        (void)library_.add("antennas", antenna("hc-ground", 0.5), "test");
        (void)library_.add("antennas", antenna("hc-air", 0.0), "test");
        rfmap::test::writeCog(dir_ / "dem.cog.tif", kWest, kNorth, kPx, kCols, kRows, flatWithHole);
        dem_ = std::make_shared<DemSource>(std::make_shared<GdalRaster>(dir_ / "dem.cog.tif", blocks_), "Flat test DEM", 3.0,
                                           "synthetic, stated LE90");
        links_ = std::make_unique<LinkService>(dem_, nullptr, library_, *builtins_, nullptr, "test");
        log_.setQuiet(true);
        host_ = std::make_unique<ServerHostServices>(nullptr, nullptr, nullptr, nullptr, log_);
        host_->bind(links_.get(), &library_, &weather_, [](const json&) {
            CalcContext ctx;
            ctx.time = Clock::now();
            return ctx;
        }, &schemas_);
    }

    [[nodiscard]] ServerHostServices& host() { return *host_; }
    [[nodiscard]] int forecastFetches() const { return forecast_.fetches; }
    [[nodiscard]] TimePoint forecastHour() const { return forecast_.hour; }

private:
    /// Empties the database (a rerun keeps the cluster) before the pool connects to it.
    static std::string prepare(const std::string& url) {
        pqxx::connection c(url);
        pqxx::work w(c);
        w.exec("drop schema public cascade; create schema public;");
        w.commit();
        return url;
    }

    fs::path dir_;
    Database db_;
    SchemaRegistry schemas_;
    Library library_;
    std::unique_ptr<BuiltinModules> builtins_;  ///< reads its table, so built after the migration
    BlockCache blocks_;
    std::shared_ptr<DemSource> dem_;
    std::unique_ptr<LinkService> links_;
    SystemClock clock_;
    NowForecast forecast_;
    Config::Weather weatherConfig_;
    WeatherService weather_;
    Logger log_;
    std::unique_ptr<ServerHostServices> host_;
};

class HostServices : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (const char* url = std::getenv("RFMAP_TEST_DATABASE_URL")) rig_ = std::make_unique<HostRig>(url);
    }
    static void TearDownTestSuite() { rig_.reset(); }

    void SetUp() override {
        if (!rig_) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
    }

    static json call(const std::string& method, const json& request) { return rig_->host().call("fpv.test", method, request); }

    /// A mission the server's own schema accepts, with a control link from the seeded library.
    static json mission() {
        return {{"schema", "mission/v1"},
                {"name", "Host calls"},
                {"time", {{"mode", "now"}}},
                {"pilot", {{"lat", 48.55}, {"lon", 35.18}, {"aglM", 1.5}}},
                {"waypoints", {{{"lat", 48.56}, {"lon", 35.19}, {"aglM", 80.0}}}},
                {"returnToPilot", true},
                {"links", {{"control", {{"radio", "elrs-915-50hz"}, {"txPowerDbm", 27}, {"groundAntenna", "hc-ground"},
                                        {"airAntenna", "hc-air"}, {"requiredMarginDb", 10}}}}},
                {"evaluate", "control"}};
    }

    static std::unique_ptr<HostRig> rig_;
};

std::unique_ptr<HostRig> HostServices::rig_;

}  // namespace

TEST_F(HostServices, ElevationPointReadsTheDemAndReportsHoles) {
    const json ground = call("elevation.point", point(48.55, 35.18));
    EXPECT_DOUBLE_EQ(ground.at("groundMslM").get<double>(), 200.0);
    EXPECT_DOUBLE_EQ(ground.at("uncertaintyM").get<double>(), 3.0);
    EXPECT_EQ(ground.at("source"), "Flat test DEM");
    EXPECT_TRUE(call("elevation.point", point(48.51, 35.23)).at("groundMslM").is_null());
}

TEST_F(HostServices, ProfileGetListsSamplesAndMissingSpans) {
    const json full = call("profile.get", {{"from", point(48.55, 35.18)}, {"to", point(48.55, 35.19)}});
    EXPECT_TRUE(full.at("missing").empty());
    ASSERT_GT(full.at("samples").size(), 2u);
    for (const json& s : full.at("samples")) EXPECT_DOUBLE_EQ(s.at("groundMslM").get<double>(), 200.0);
    EXPECT_DOUBLE_EQ(full.at("samples").front().at("distanceM").get<double>(), 0.0);

    // North to south through the hole and out of the raster's southern edge: one span, to the end.
    const json holed = call("profile.get", {{"from", point(48.53, 35.23)}, {"to", point(48.50, 35.23)}, {"stepM", 50.0}});
    ASSERT_EQ(holed.at("missing").size(), 1u);
    EXPECT_GT(holed.at("missing").at(0).at("fromM").get<double>(), 0.0);
    EXPECT_NEAR(holed.at("stepM").get<double>(), 50.0, 1.0);

    // Out of the hole again: the span is closed where data comes back.
    const json through = call("profile.get", {{"from", point(48.53, 35.23)}, {"to", point(48.53, 35.20)}});
    EXPECT_TRUE(through.at("missing").empty());
    const json across = call("profile.get", {{"from", point(48.515, 35.25)}, {"to", point(48.515, 35.20)}});
    ASSERT_EQ(across.at("missing").size(), 1u);
    EXPECT_DOUBLE_EQ(across.at("missing").at(0).at("fromM").get<double>(), 0.0);
    EXPECT_GT(across.at("missing").at(0).at("toM").get<double>(), 0.0);

    EXPECT_THROW(call("profile.get", {{"from", point(48.55, 35.18)}, {"to", point(48.55, 35.19)}, {"stepM", 0.0}}),
                 HostCallError);
}

TEST_F(HostServices, LibraryGetReturnsItemsAndRefusesUnknownOnes) {
    const json radio = call("library.get", {{"kind", "radio-profiles"}, {"id", "elrs-915-50hz"}});
    EXPECT_EQ(radio.at("id"), "elrs-915-50hz");
    EXPECT_GT(radio.at("frequencyHz").get<double>(), 9e8);
    try {
        (void)call("library.get", {{"kind", "antennas"}, {"id", "nope"}});
        FAIL() << "expected a host call error";
    } catch (const HostCallError& e) {
        EXPECT_NE(std::string(e.what()).find("antennas/nope"), std::string::npos);
    }
}

TEST_F(HostServices, WeatherPointReadsTheForecast) {
    // Without a time the hour nearest to now is read, which may be either side of the middle one.
    const json now = call("weather.point", point(48.55, 35.18));
    EXPECT_TRUE(now.at("temperatureC").is_number());
    const json middle = call("weather.point", {{"lat", 48.55}, {"lon", 35.18}, {"time", isoUtc(rig_->forecastHour())}});
    EXPECT_DOUBLE_EQ(middle.at("temperatureC").get<double>(), 10.0);
    EXPECT_GE(rig_->forecastFetches(), 1);
    EXPECT_THROW(call("weather.point", {{"lat", 48.55}, {"lon", 35.18}, {"time", "yesterday"}}), HostCallError);
}

TEST_F(HostServices, LinkEvaluateWithTheMissionShortcut) {
    const json r = call("link.evaluate", {{"mission", mission()}, {"link", "control"},
                                          {"drone", {{"lat", 48.56}, {"lon", 35.19}, {"aglM", 80.0}}}});
    EXPECT_TRUE(r.at("marginDb").is_number());
    EXPECT_GT(r.at("marginDb").get<double>(), 0.0);

    // A link the mission does not set up, a mission the schema refuses, and a drone over the hole.
    EXPECT_THROW(call("link.evaluate", {{"mission", mission()}, {"link", "video"}, {"drone", point(48.56, 35.19)}}),
                 HostCallError);
    json bad = mission();
    bad.erase("pilot");
    try {
        (void)call("link.evaluate", {{"mission", bad}, {"link", "control"}, {"drone", point(48.56, 35.19)}});
        FAIL() << "expected a host call error";
    } catch (const HostCallError& e) {
        EXPECT_NE(std::string(e.what()).find("bad_request"), std::string::npos);
    }
    try {
        (void)call("link.evaluate", {{"mission", mission()}, {"link", "control"},
                                     {"drone", {{"lat", 48.51}, {"lon", 35.23}, {"aglM", 80.0}}}});
        FAIL() << "expected a host call error";
    } catch (const HostCallError& e) {
        EXPECT_NE(std::string(e.what()).find("missing-data"), std::string::npos);
    }
}

TEST_F(HostServices, LinkEvaluateWithRawRadioParameters) {
    const json req = {{"frequencyHz", 9.15e8},
                      {"txPowerDbm", 27.0},
                      {"time", "2026-10-18T12:00:00Z"},
                      {"tx", {{"lat", 48.55}, {"lon", 35.18}, {"aglM", 2.0}, {"gainDbi", 2.15}, {"feederLossDb", 0.5}}},
                      {"rx", {{"lat", 48.56}, {"lon", 35.19}, {"aglM", 80.0}, {"sensitivityDbm", -105.0}}}};
    const json r = call("link.evaluate", req);
    EXPECT_TRUE(r.at("marginDb").is_number());

    json badTime = req;
    badTime["time"] = "noon";
    EXPECT_THROW(call("link.evaluate", badTime), HostCallError);

    json overHole = req;
    overHole["rx"]["lat"] = 48.51;
    overHole["rx"]["lon"] = 35.23;
    try {
        (void)call("link.evaluate", overHole);
        FAIL() << "expected a host call error";
    } catch (const HostCallError& e) {
        EXPECT_NE(std::string(e.what()).find("missing-data"), std::string::npos);
    }
}

TEST_F(HostServices, ModuleLogLinesGoToTheServerLog) {
    // Every level maps to a server level; none throws.
    for (const int level : {RFMOD_LOG_DEBUG, RFMOD_LOG_INFO, RFMOD_LOG_WARN, RFMOD_LOG_ERROR}) {
        EXPECT_NO_THROW(rig_->host().log("fpv.test", level, "line"));
    }
}
