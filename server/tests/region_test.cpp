// Offline regions: definitions, the style of a package, the pmtiles call, and the catalog building,
// keeping and cleaning packages (with a stand-in for go-pmtiles).

#include "region_api.hpp"
#include "region_catalog.hpp"
#include "region_test_support.hpp"
#include "request.hpp"
#include "security.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <fstream>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

json square(double lon, double lat, double size) {
    return {{"type", "Polygon"},
            {"coordinates",
             {{{lon, lat}, {lon + size, lat}, {lon + size, lat + size}, {lon, lat + size}, {lon, lat}}}}};
}

RegionDefinition define(const json& entry, const fs::path& base = fs::current_path()) { return {entry, base}; }

}  // namespace

TEST(RegionDefinition, ReadsBoxPolygonAndOptions) {
    const RegionDefinition box = define({{"id", "kharkiv"}, {"name", "Kharkiv"}, {"bbox", {34.8, 48.3, 37.0, 50.5}}});
    EXPECT_EQ(box.name(), "Kharkiv");
    EXPECT_FALSE(box.area().polygon());
    EXPECT_EQ(box.area().box().toArgument(), "34.8,48.3,37,50.5");

    const RegionDefinition poly = define({{"id", "p1"},
                                          {"geometry", {{"type", "Feature"}, {"geometry", square(35.0, 48.0, 0.5)}}},
                                          {"maxzoom", 14},
                                          {"layers", {"terrain", "basemap", "basemap"}},
                                          {"glyphRanges", {"1024-1279", "0-255"}}});
    EXPECT_EQ(poly.name(), "p1");
    ASSERT_TRUE(poly.area().polygon());
    EXPECT_EQ((*poly.area().polygon())["type"], "MultiPolygon");
    EXPECT_EQ(poly.area().box().toJson(), json({35.0, 48.0, 35.5, 48.5}));
    EXPECT_EQ(poly.maxZoom(), 14);
    EXPECT_EQ(poly.layers(), (std::vector<std::string>{"basemap", "terrain"}));
    EXPECT_EQ(poly.glyphRanges(), (std::vector<std::string>{"0-255", "1024-1279"}));
}

TEST(RegionDefinition, RefusesWhatIsWrong) {
    const auto refused = [](const json& entry) {
        EXPECT_THROW((void)define(entry), RegionError) << entry.dump();
    };
    refused({{"id", "Upper"}, {"bbox", {0, 0, 1, 1}}});
    refused({{"id", "-dash"}, {"bbox", {0, 0, 1, 1}}});
    refused({{"id", "x"}});
    refused({{"id", "x"}, {"bbox", {1, 0, 0, 1}}});
    refused({{"id", "x"}, {"bbox", {0, 0, 1, 1}}, {"geometry", square(0, 0, 1)}});
    refused({{"id", "x"}, {"bbox", {0, 0, 1, 1}}, {"maxzoom", 30}});
    refused({{"id", "x"}, {"bbox", {0, 0, 1, 1}}, {"layers", {"roads"}}});
    refused({{"id", "x"}, {"bbox", {0, 0, 1, 1}}, {"glyphRanges", {"latin"}}});
    refused({{"id", "x"}, {"geometry", {{"type", "Point"}, {"coordinates", {0, 0}}}}});
    refused({{"id", "x"}, {"geometry", {{"type", "Polygon"}, {"coordinates", {{{0, 0}, {1, 0}, {1, 1}}}}}}});
    refused({{"id", "x"}, {"boundary", "../outside.geojson"}});
    refused({{"id", "x"}, {"boundary", "/etc/passwd"}});
}

TEST(RegionDefinition, BoundaryFileIsReadBesideTheDefinitions) {
    const fs::path dir = test::scratchDir("region-definitions");
    fs::create_directories(dir / "boundaries");
    std::ofstream(dir / "boundaries" / "b.geojson")
        << json{{"type", "FeatureCollection"},
                {"features", {{{"type", "Feature"}, {"geometry", square(30.0, 50.0, 1.0)}}}}}.dump();
    std::ofstream(dir / "regions.json")
        << json{{"regions",
                 {{{"id", "b"}, {"name", "B"}, {"boundary", "boundaries/b.geojson"}},
                  {{"id", "a"}, {"bbox", {0, 0, 1, 1}}},
                  {{"id", "a"}, {"bbox", {0, 0, 2, 2}}},
                  {{"id", "bad"}}}}}.dump();
    const RegionDefinitions file(dir / "regions.json");
    const auto& regions = file.regions();
    std::vector<std::string> skipped;
    for (const auto& s : file.skipped()) skipped.push_back(s.id);
    ASSERT_EQ(regions.size(), 2U);
    EXPECT_EQ(regions[0].id(), "a");
    EXPECT_EQ(regions[0].area().box().toJson(), json({0, 0, 1, 1}));
    EXPECT_EQ(regions[1].area().box().toJson(), json({30.0, 50.0, 31.0, 51.0}));
    EXPECT_EQ(skipped, (std::vector<std::string>{"a", "bad"}));
    EXPECT_TRUE(RegionDefinitions(dir / "missing.json").regions().empty());
}

TEST(RegionStyle, PointsAtThePackageAndDropsWhatItLacks) {
    RegionPackage p;
    p.id = "r";
    p.bbox = GeoBox(30, 50, 32, 52);
    p.layers = {"basemap", "hillshade", "terrain"};
    const json style = RegionStyle::rewrite(test::TestWorld::style(), p, false);
    EXPECT_EQ(style["sources"]["openmaptiles"]["url"], "pmtiles://tiles/basemap.pmtiles");
    EXPECT_EQ(style["sources"]["hillshade"]["url"], "pmtiles://tiles/hillshade.pmtiles");
    EXPECT_FALSE(style["sources"].contains("satellite"));
    EXPECT_EQ(style["sources"]["terrain"]["type"], "raster-dem");
    for (const json& layer : style["layers"]) EXPECT_NE(layer.value("source", ""), "satellite");
    EXPECT_EQ(style["glyphs"], "glyphs/{fontstack}/{range}.pbf");
    EXPECT_FALSE(style.contains("sprite"));
    EXPECT_EQ(style["center"], json({31.0, 51.0}));
    EXPECT_EQ(RegionStyle::fontstacks(style), (std::vector<std::string>{"Noto Sans Medium", "Noto Sans Regular"}));
    EXPECT_EQ(RegionStyle::rewrite(test::TestWorld::style(), p, true)["sprite"], "sprites/sprite");
}

TEST(PmtilesTool, CutsByBoxOrPolygonWithoutAShell) {
    PmtilesTool::Extract job{"/w/basemap.pmtiles", "/p/basemap.pmtiles", GeoBox(1, 2, 3, 4), std::nullopt, 12};
    EXPECT_EQ(PmtilesTool::extractArguments(job), (std::vector<std::string>{"extract", "/w/basemap.pmtiles", "/p/basemap.pmtiles",
                                                                            "--quiet", "--bbox=1,2,3,4", "--maxzoom=12"}));
    job.region = "/p/area.geojson";
    job.maxZoom.reset();
    EXPECT_EQ(PmtilesTool::extractArguments(job).back(), "--region=/p/area.geojson");
    EXPECT_THROW((void)PmtilesTool("pmtiles").version("/dev/null"), RegionError);  // not an absolute path
}

TEST(HttpRequest, IfNoneMatchNamesTheCurrentTag) {
    httplib::Request raw;
    const Config::Server server;
    const HttpRequest req(raw, server);
    EXPECT_FALSE(req.holdsCurrent("\"a\""));
    raw.set_header("If-None-Match", "\"b\", W/\"a\"");
    EXPECT_TRUE(req.holdsCurrent("\"a\""));
    EXPECT_FALSE(req.holdsCurrent("\"c\""));
    raw.headers.clear();
    raw.set_header("If-None-Match", "*");
    EXPECT_TRUE(req.holdsCurrent("\"c\""));
}

class RegionCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // One directory per test: ctest runs the tests of this fixture in parallel processes.
        dir_ = test::scratchDir(std::string("region-catalog-") +
                                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        world_ = std::make_unique<test::TestWorld>(dir_ / "world");
        log_.setQuiet(true);
        define(json::array({{{"id", "alpha"}, {"name", "Alpha"}, {"bbox", {35.1, 48.5, 35.3, 48.7}}, {"glyphRanges", {"1024-1279"}}},
                            {{"id", "beta"}, {"geometry", square(35.0, 48.5, 0.2)}, {"layers", {"basemap"}}, {"maxzoom", 13}}}));
    }

    void define(const json& regions) const { std::ofstream(dir_ / "regions.json", std::ios::trunc) << json{{"regions", regions}}.dump(); }

    [[nodiscard]] RegionCatalog::Settings settings(const fs::path& tool = FAKE_PMTILES) const {
        return {dir_ / "world", dir_ / "regions.json", dir_ / "packages", tool, 0, false};
    }

    [[nodiscard]] std::string extractLog() const {
        std::ifstream in(dir_ / "world" / "extract.log");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    fs::path dir_;
    std::unique_ptr<test::TestWorld> world_;
    Logger log_;
    test::FakeClock clock_;
};

TEST_F(RegionCatalogTest, BuildsAPackageMapLibreCanUseOffline) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const auto alpha = catalog.find("alpha");
    ASSERT_TRUE(alpha);
    EXPECT_EQ(alpha->layers, (std::vector<std::string>{"basemap", "hillshade", "terrain"}));
    EXPECT_TRUE(alpha->preview);
    EXPECT_TRUE(fs::is_regular_file(catalog.store().preview(*alpha)));

    const test::ZipContents zip(catalog.store().archive(*alpha));
    ASSERT_FALSE(zip.names.empty());
    EXPECT_EQ(zip.names.front(), "manifest.json");
    const std::vector<std::string> rest(zip.names.begin() + 1, zip.names.end());
    EXPECT_EQ(rest, (std::vector<std::string>{"glyphs/Noto Sans Medium/1024-1279.pbf", "glyphs/Noto Sans Regular/1024-1279.pbf",
                                              "sprites/sprite.json", "sprites/sprite.png", "sprites/sprite@2x.json",
                                              "sprites/sprite@2x.png", "style.json", "tiles/basemap.pmtiles",
                                              "tiles/hillshade.pmtiles", "tiles/terrain.pmtiles"}));
    EXPECT_EQ(zip.files.at("tiles/basemap.pmtiles"), "PMTiles basemap");
    const json manifest = json::parse(zip.files.at("manifest.json"));
    EXPECT_EQ(manifest["version"], alpha->version);
    EXPECT_EQ(manifest["bbox"], json({35.1, 48.5, 35.3, 48.7}));
    EXPECT_EQ(manifest["files"].size(), rest.size());
    EXPECT_EQ(manifest["files"][7]["sha256"], Secrets::hex(Secrets::sha256("PMTiles basemap")));
    EXPECT_EQ(json::parse(zip.files.at("style.json"))["sprite"], "sprites/sprite");

    const auto beta = catalog.find("beta");
    ASSERT_TRUE(beta);
    EXPECT_EQ(beta->layers, std::vector<std::string>{"basemap"});
    EXPECT_EQ(beta->maxZoom, 13);
    const std::string log = extractLog();
    EXPECT_NE(log.find("--bbox=35.1,48.5,35.3,48.7"), std::string::npos) << log;
    EXPECT_NE(log.find("--region="), std::string::npos) << log;
    EXPECT_NE(log.find("--maxzoom=13"), std::string::npos) << log;
}

TEST_F(RegionCatalogTest, RebuildsOnlyWhatChangedAndKeepsTheVersionOfTheSameFiles) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const RegionPackage first = *catalog.find("alpha");
    clock_.advance(std::chrono::seconds(60));
    catalog.scan();
    EXPECT_EQ(catalog.find("alpha")->builtAt, first.builtAt) << "nothing changed: no rebuild";

    // The same bytes written again: built again, but the version stays.
    world_->write("basemap.pmtiles", "PMTiles basemap");
    fs::last_write_time(world_->dir() / "basemap.pmtiles", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    catalog.scan();
    EXPECT_NE(catalog.find("alpha")->builtAt, first.builtAt);
    EXPECT_EQ(catalog.find("alpha")->version, first.version);

    // New tiles: a new version, and the old zip is gone.
    world_->write("basemap.pmtiles", "PMTiles basemap, newer");
    fs::last_write_time(world_->dir() / "basemap.pmtiles", fs::file_time_type::clock::now() + std::chrono::seconds(10));
    catalog.scan();
    const RegionPackage second = *catalog.find("alpha");
    EXPECT_NE(second.version, first.version);
    EXPECT_FALSE(fs::exists(catalog.store().archive(first)));
    EXPECT_TRUE(fs::exists(catalog.store().archive(second)));

    // A second process on the same directories reuses the packages.
    RegionCatalog other(settings(), log_, clock_);
    other.scan();
    EXPECT_EQ(other.find("alpha")->builtAt, second.builtAt);
}

TEST_F(RegionCatalogTest, ANewNameIsANewVersion) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const RegionPackage first = *catalog.find("alpha");
    define(json::array({{{"id", "alpha"}, {"name", "Alpha, renamed"}, {"bbox", {35.1, 48.5, 35.3, 48.7}}, {"glyphRanges", {"1024-1279"}}}}));
    catalog.scan();
    const RegionPackage second = *catalog.find("alpha");
    EXPECT_NE(second.version, first.version) << "the manifest in the zip changed, so its URL must change too";
    const test::ZipContents zip(catalog.store().archive(second));
    EXPECT_EQ(json::parse(zip.files.at("manifest.json"))["name"], "Alpha, renamed");
}

TEST_F(RegionCatalogTest, ARegionOutsideTheDemHasNoPreview) {
    define(json::array({{{"id", "far"}, {"bbox", {10.0, 40.0, 10.2, 40.2}}}}));
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const auto far = catalog.find("far");
    ASSERT_TRUE(far);
    EXPECT_FALSE(far->preview);
    EXPECT_FALSE(fs::exists(catalog.store().preview(*far)));
}

TEST_F(RegionCatalogTest, KeepsTheLastGoodPackageWhenABuildFailsAndRemovesUndefinedRegions) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const RegionPackage beta = *catalog.find("beta");
    define(json::array({{{"id", "beta"}, {"geometry", square(35.0, 48.5, 0.2)}, {"layers", {"basemap"}}, {"maxzoom", 0}}}));
    catalog.scan();
    const auto statuses = catalog.statuses();
    ASSERT_EQ(statuses.size(), 1U);
    EXPECT_EQ(statuses[0].state, RegionState::Failed);
    EXPECT_NE(statuses[0].error.find("fake failure"), std::string::npos) << statuses[0].error;
    ASSERT_TRUE(statuses[0].package);
    EXPECT_EQ(statuses[0].package->version, beta.version);
    EXPECT_FALSE(catalog.find("alpha"));
    for (const auto& entry : fs::directory_iterator(dir_ / "packages")) {
        EXPECT_FALSE(entry.path().filename().string().starts_with("alpha")) << entry.path();
        EXPECT_FALSE(entry.path().filename().string().starts_with(".work-")) << entry.path();
    }
}

TEST_F(RegionCatalogTest, AnUnreadableDefinitionsFileKeepsEveryPackage) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const RegionPackage alpha = *catalog.find("alpha");
    std::ofstream(dir_ / "regions.json", std::ios::trunc) << "{\"regions\": [";  // a typo while editing
    catalog.scan();
    EXPECT_EQ(catalog.packages().size(), 2U);
    EXPECT_TRUE(fs::exists(catalog.store().archive(alpha)));

    // An entry with an id of the wrong type is left out like any other wrong entry.
    define(json::array({{{"id", 5}, {"bbox", {35.1, 48.5, 35.3, 48.7}}},
                        {{"id", "alpha"}, {"name", "Alpha"}, {"bbox", {35.1, 48.5, 35.3, 48.7}}, {"glyphRanges", {"1024-1279"}}}}));
    catalog.scan();
    EXPECT_EQ(catalog.statuses().size(), 1U);
    EXPECT_EQ(catalog.find("alpha")->version, alpha.version);
}

TEST_F(RegionCatalogTest, AWorldStyleOfAnUnexpectedShapeFailsTheRegionOnly) {
    world_->write("style-dark.json", R"({"version": 8, "metadata": "not an object", "sources": {}, "layers": []})");
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const auto statuses = catalog.statuses();
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].state, RegionState::Failed);
    EXPECT_FALSE(statuses[0].error.empty());
}

TEST_F(RegionCatalogTest, ANewGlyphFileRebuildsThePackages) {
    RegionCatalog catalog(settings(), log_, clock_);
    catalog.scan();
    const RegionPackage first = *catalog.find("alpha");
    clock_.advance(std::chrono::seconds(60));
    const fs::path fontstack = world_->dir() / "fonts" / "Noto Sans Regular";
    std::ofstream(fontstack / "256-511.pbf") << "glyphs";
    fs::last_write_time(fontstack, fs::file_time_type::clock::now() + std::chrono::seconds(5));
    catalog.scan();
    EXPECT_NE(catalog.find("alpha")->builtAt, first.builtAt);
}

TEST_F(RegionCatalogTest, WithoutTheToolEveryRegionFailsAndNothingIsBuilt) {
    RegionCatalog catalog(settings(dir_ / "no-such-pmtiles"), log_, clock_);
    catalog.scan();
    EXPECT_TRUE(catalog.packages().empty());
    const auto statuses = catalog.statuses();
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].state, RegionState::Failed);
    EXPECT_NE(statuses[0].error.find("regions.pmtiles"), std::string::npos);
}

TEST(RegionCatalog, IsOffWithoutAWorldDirectory) {
    Logger log;
    const test::FakeClock clock;
    RegionCatalog catalog({}, log, clock);
    EXPECT_FALSE(catalog.enabled());
    catalog.scan();
    catalog.reload();
    EXPECT_TRUE(catalog.packages().empty());
    EXPECT_EQ(catalog.scans(), 0U);
}

TEST(RegionApi, StreamsAQuarterOfTheRequestThreadsAtOnceUnlessConfigured) {
    Logger log;
    const test::FakeClock clock;
    RegionCatalog catalog({}, log, clock);
    const AdminToken admin("");
    EXPECT_EQ(RegionApi(catalog, admin, 0, 16).downloadGate().limits().concurrency, 4);
    EXPECT_EQ(RegionApi(catalog, admin, 0, 2).downloadGate().limits().concurrency, 1);
    EXPECT_EQ(RegionApi(catalog, admin, 7, 16).downloadGate().limits().concurrency, 7);
    EXPECT_EQ(RegionApi(catalog, admin, 1, 16).downloadGate().limits().queue, 0) << "a busy client retries, it does not wait";
}
