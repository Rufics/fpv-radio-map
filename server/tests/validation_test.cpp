#include "config.hpp"
#include "schemas.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>

using namespace rfmap;

namespace {

json goodMission() {
    return json::parse(R"({
      "schema": "mission/v1", "name": "Ridge", "time": {"mode": "now"},
      "pilot": {"lat": 48.53, "lon": 35.2, "aglM": 1.5},
      "waypoints": [{"lat": 48.54, "lon": 35.21, "aglM": 100}],
      "links": {"control": {"radio": "elrs-915-50hz", "txPowerDbm": 30, "groundAntenna": "g", "airAntenna": "a",
                            "requiredMarginDb": 10}},
      "evaluate": "control"})");
}

class ValidationTest : public ::testing::Test {
protected:
    SchemaRegistry schemas{RFMAP_SCHEMAS_DIR};

    std::vector<std::string> pointers(const std::string& name, const json& doc) {
        std::vector<std::string> out;
        for (const auto& p : schemas.check(name, doc)) out.push_back(p.pointer);
        return out;
    }
};

}  // namespace

TEST_F(ValidationTest, ValidMissionPasses) { EXPECT_TRUE(schemas.check("mission", goodMission()).empty()); }

TEST_F(ValidationTest, MissionProblemsNameTheField) {
    json m = goodMission();
    m["pilot"].erase("aglM");
    m["waypoints"][0]["lat"] = 91;
    m["links"]["control"]["requiredMarginDb"] = -1;
    const auto p = pointers("mission", m);
    EXPECT_NE(std::find(p.begin(), p.end(), "/pilot"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "/waypoints/0/lat"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "/links/control/requiredMarginDb"), p.end());

    json noWaypoints = goodMission();
    noWaypoints["waypoints"] = json::array();
    EXPECT_FALSE(schemas.check("mission", noWaypoints).empty());
    json wrongSchema = goodMission();
    wrongSchema["schema"] = "mission/v2";
    EXPECT_FALSE(schemas.check("mission", wrongSchema).empty());
    json badTime = goodMission();
    badTime["time"] = {{"mode", "planned"}, {"utc", "tomorrow"}};
    EXPECT_FALSE(schemas.check("mission", badTime).empty());
}

TEST_F(ValidationTest, LongFormattedStringIsRefusedNotMatched) {
    // The format check's regex overflowed the stack on long strings (found by fuzzing).
    json m = goodMission();
    m["time"] = {{"mode", "planned"}, {"utc", "2026-10-18T14:00:00." + std::string(200000, '1') + "Z"}};
    const auto p = pointers("mission", m);
    EXPECT_NE(std::find(p.begin(), p.end(), "/time/utc"), p.end());
}

TEST_F(ValidationTest, RequireThrows422WithTheReport) {
    json m = goodMission();
    m.erase("evaluate");
    try {
        schemas.require("mission", m);
        FAIL();
    } catch (const HttpError& e) {
        EXPECT_EQ(e.status(), 422);
        ASSERT_TRUE(e.body().contains("issues"));
        EXPECT_FALSE(e.body()["issues"].empty());
    }
}

TEST_F(ValidationTest, SeedRadioProfilesValidateWithCrossFileRefs) {
    std::ifstream in(RFMAP_SEED_DIR "/radio-profiles/seed.json");
    const json seed = json::parse(in);
    ASSERT_FALSE(seed.empty());
    for (const json& item : seed) EXPECT_TRUE(schemas.check("radio-profiles", item).empty()) << item.at("id");
    json bad = seed.at(0);
    bad["id"] = "Bad Id";  // common.schema.json#/$defs/id
    bad["sensitivity"] = {{"method", "datasheet"}};
    const auto p = pointers("radio-profiles", bad);
    EXPECT_NE(std::find(p.begin(), p.end(), "/id"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "/sensitivity"), p.end());
}

TEST_F(ValidationTest, EverySeedFileValidatesWithUniqueIds) {
    for (const std::string& kind : SchemaRegistry::libraryKinds()) {
        const std::filesystem::path kindDir = std::filesystem::path(RFMAP_SEED_DIR) / kind;
        if (!std::filesystem::is_directory(kindDir)) continue;
        std::set<std::string> ids;  // unique across all of the kind's seed files
        for (const auto& entry : std::filesystem::directory_iterator(kindDir)) {
            const std::string name = entry.path().filename().string();
            if (!name.starts_with("seed") || !name.ends_with(".json")) continue;
            std::ifstream in(entry.path());
            const json seed = json::parse(in, nullptr, false);
            ASSERT_TRUE(seed.is_array()) << entry.path() << " is not a JSON array";
            for (const json& item : seed) {
                // Typed access throws on a wrong type, so check the type first and let the schema report it.
                const bool hasId = item.contains("id") && item.at("id").is_string();
                const std::string id = hasId ? item.at("id").get<std::string>() : "<no string id>";
                EXPECT_TRUE(schemas.check(kind, item).empty()) << kind << "/" << id;
                EXPECT_TRUE(ids.insert(id).second) << kind << " repeats '" << id << "'";
                EXPECT_TRUE(item.contains("verified") && item.at("verified") == false) << kind << "/" << id;
            }
        }
    }
}

TEST_F(ValidationTest, AntennaSchema) {
    const json ok = {{"id", "ground-dipole"}, {"name", "Dipole"}, {"version", 1}, {"verified", false},
                     {"type", "dipole"}, {"gainDbi", 2.15}, {"polarization", "linear-vertical"},
                     {"bandHz", {9e8, 9.3e8}}, {"source", {{"kind", "standard"}, {"reference", "half-wave dipole"}}}};
    EXPECT_TRUE(schemas.check("antennas", ok).empty());
    json extra = ok;
    extra["magic"] = true;  // additionalProperties: false
    EXPECT_FALSE(schemas.check("antennas", extra).empty());
}

TEST_F(ValidationTest, InlineRequestSchemaCanReferToTheMissionFile) {
    const std::string m = "https://rfmap.local/schemas/mission.schema.json#/$defs/";
    schemas.addInline("pair", {{"type", "object"},
                               {"required", {"from", "to"}},
                               {"properties", {{"from", {{"$ref", m + "point"}}}, {"to", {{"$ref", m + "point"}}}}}});
    EXPECT_TRUE(schemas.check("pair", {{"from", {{"lat", 1}, {"lon", 2}, {"aglM", 0}}},
                                       {"to", {{"lat", 1}, {"lon", 2.1}, {"aglM", 50}}}})
                    .empty());
    EXPECT_FALSE(schemas.check("pair", {{"from", {{"lat", 1}, {"lon", 2}, {"aglM", -1}}},
                                        {"to", {{"lat", 1}, {"lon", 2.1}, {"aglM", 50}}}})
                     .empty());
}

TEST(ConfigTest, EnvironmentOverridesTheFile) {
    const auto file = rfmap::test::scratchDir("config") / "rfmap-config-test.json";
    {
        std::ofstream out(file);
        out << R"({"server": {"port": 9000}, "cache": {"demBlocksMb": 8}, "admin": {"token": "from-file"}})";
    }
    const Config c = Config::load(file, {{"RFMAP_ADMIN_TOKEN", "from-env"},
                                         {"RFMAP__CACHE__DEMBLOCKSMB", "32"},
                                         {"RFMAP__WEATHER__BASEURL", "https://weather.example"},
                                         {"RFMAP_PORT", "8181"}});
    EXPECT_EQ(c.adminToken, "from-env");
    EXPECT_EQ(c.cache.demBlocksMb, 32u);
    EXPECT_EQ(c.weather.baseUrl, "https://weather.example");
    EXPECT_EQ(c.server.port, 8181);
    EXPECT_EQ(c.raw["admin"]["token"], "(set)");  // never echoed
    std::filesystem::remove(file);
    EXPECT_THROW(Config::fromJson({{"server", {{"port", 70000}}}}), std::invalid_argument);
}

TEST(ConfigTest, MetricsAreOffUnlessEnabled) {
    EXPECT_FALSE(Config::fromJson(json::object()).metrics.enabled);
    EXPECT_TRUE(Config::fromJson({{"metrics", {{"enabled", true}}}}).metrics.enabled);
    const auto file = rfmap::test::scratchDir("config") / "rfmap-metrics-config-test.json";
    std::ofstream(file) << "{}";
    EXPECT_TRUE(Config::load(file, {{"RFMAP__METRICS__ENABLED", "true"}}).metrics.enabled);
}

TEST(ConfigTest, ShippedExampleLoads) {
    // deploy/docker-compose.yml mounts this file; it must stay loadable.
    const Config c = Config::load(RFMAP_EXAMPLE_CONFIG, {{"RFMAP_ADMIN_TOKEN", "x"}});
    EXPECT_EQ(c.server.port, 8080);
    EXPECT_EQ(c.weather.baseUrl, "http://open-meteo:8080");
    EXPECT_FALSE(c.data.demStatedErrorM.has_value());
    EXPECT_EQ(c.rateLimits.at("open").burst, 5.0);
    EXPECT_EQ(c.raw["map"]["zoom"], 11);
}

TEST(TimeTest, IsoParsing) {
    const auto t = UtcTime::parse("2026-10-18T14:00:00Z");
    ASSERT_TRUE(t);
    EXPECT_EQ(UtcTime::format(*t), "2026-10-18T14:00:00Z");
    EXPECT_EQ(UtcTime::format(*UtcTime::parse("2026-10-18T16:30:00+02:30")), "2026-10-18T14:00:00Z");
    EXPECT_FALSE(UtcTime::parse("2026-02-31T00:00:00Z"));
    EXPECT_FALSE(UtcTime::parse("yesterday"));
    EXPECT_EQ(UtcTime::format(*UtcTime::parse("2026-10-18 14:00z")), "2026-10-18T14:00:00Z");
    EXPECT_EQ(*UtcTime::parse("2026-10-18T14:00:00.5-0130") - *UtcTime::parse("2026-10-18T15:30:00Z"),
              std::chrono::milliseconds(500));
    for (const char* bad : {"2026-10-18T14:00:00", "2026-10-18T14:00:00.Z", "2026-1-18T14:00Z", "2026-10-18T24:00Z",
                            "2026-10-18T14:00:00Z ", "2026-10-18T14:00+0", "２026-10-18T14:00Z",
                            // Years the clock cannot hold, also when an offset moves the time there.
                            "0999-01-01T00:00Z", "2602-10-18T14:00Z", "1900-01-01T00:00+02:00",
                            "2200-12-31T23:00-02:00"}) {
        EXPECT_FALSE(UtcTime::parse(bad)) << bad;
    }
}

TEST(TimeTest, LongFractionDoesNotOverflowTheStack) {
    // A regex matched this one frame per character and crashed the server (found by fuzzing).
    const auto t = UtcTime::parse("2026-10-18T14:00:00." + std::string(200000, '1') + "Z");
    ASSERT_TRUE(t);
    EXPECT_EQ(UtcTime::format(*t), "2026-10-18T14:00:00Z");
}
