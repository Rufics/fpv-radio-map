// Drawn areas: their size on the Earth, the checks of a request, the build queue and the
// removal of packages nobody uses (with a stand-in for go-pmtiles).

#include "region_api.hpp"
#include "region_areas.hpp"
#include "region_test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

json square(double lon, double lat, double size) {
    return {{"type", "Polygon"},
            {"coordinates",
             {{{lon, lat}, {lon + size, lat}, {lon + size, lat + size}, {lon, lat + size}, {lon, lat}}}}};
}

/// A circle-like polygon of `n` vertices around (lon, lat).
json ring(double lon, double lat, double radius, int n) {
    json positions = json::array();
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * 3.14159265358979 * i / n;
        positions.push_back({lon + radius * std::cos(a), lat + radius * std::sin(a)});
    }
    positions.push_back(positions.front());
    return {{"type", "Polygon"}, {"coordinates", {positions}}};
}

}  // namespace

TEST(RegionArea, MeasuresTheAreaOnTheEarth) {
    // One degree square at the equator: 111.195 km a side on the mean sphere.
    const RegionArea box(GeoBox(0, 0, 1, 1));
    EXPECT_NEAR(box.areaKm2(), 12364.0, 5.0);
    EXPECT_EQ(box.vertices(), 4U);
    // The same square as a polygon, either way round, and further north where degrees are shorter.
    EXPECT_NEAR(RegionArea::fromGeoJson(square(0, 0, 1)).areaKm2(), 12364.0, 15.0);
    json reversed = square(0, 0, 1);
    std::ranges::reverse(reversed["coordinates"][0]);
    EXPECT_NEAR(RegionArea::fromGeoJson(reversed).areaKm2(), 12364.0, 15.0);
    const double north = RegionArea::fromGeoJson(square(35.0, 48.0, 0.2)).areaKm2();
    EXPECT_NEAR(north, RegionArea(GeoBox(35.0, 48.0, 35.2, 48.2)).areaKm2(), 1.0);
    EXPECT_NEAR(north, 22.24 * 22.24 * std::cos(48.1 * 3.14159265358979 / 180.0), 2.0);

    // A hole is taken out; a second polygon is added.
    json holed = square(0, 0, 1);
    holed["coordinates"].push_back({{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}, {0.25, 0.25}});
    const RegionArea withHole = RegionArea::fromGeoJson(holed);
    EXPECT_NEAR(withHole.areaKm2(), 12364.0 * 0.75, 20.0);
    EXPECT_EQ(withHole.vertices(), 8U);
    const json two = {{"type", "MultiPolygon"}, {"coordinates", {square(0, 0, 1)["coordinates"], square(2, 0, 1)["coordinates"]}}};
    EXPECT_NEAR(RegionArea::fromGeoJson(two).areaKm2(), 2 * 12364.0, 30.0);
}

class RegionAreasTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = test::scratchDir(std::string("region-areas-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        world_ = std::make_unique<test::TestWorld>(dir_ / "world");
        log_.setQuiet(true);
    }

    [[nodiscard]] RegionAreas::Settings settings(RegionAreas::Limits limits = {}) const {
        return {dir_ / "world", dir_ / "areas", FAKE_PMTILES, limits, false};
    }

    [[nodiscard]] int extracts() const {
        std::ifstream in(dir_ / "world" / "extract.log");
        int lines = 0;
        for (std::string line; std::getline(in, line);) ++lines;
        return lines;
    }

    static json field(double lon = 35.1, double lat = 48.6) { return {{"name", "Field"}, {"geometry", square(lon, lat, 0.05)}}; }

    fs::path dir_;
    std::unique_ptr<test::TestWorld> world_;
    Logger log_;
    test::FakeClock clock_;
};

TEST_F(RegionAreasTest, TheSameRequestIsTheSameArea) {
    const RegionAreas areas(settings(), log_, clock_);
    const RegionDefinition a = areas.define(field());
    EXPECT_TRUE(a.id().starts_with(RegionAreas::kIdPrefix));
    EXPECT_EQ(a.id().size(), RegionAreas::kIdPrefix.size() + 24);
    EXPECT_EQ(a.name(), "Field");
    EXPECT_EQ(a.maxZoom(), 14) << "a request without a zoom gets the highest a drawn area may have";
    EXPECT_EQ(areas.define(field()).id(), a.id());
    EXPECT_EQ(areas.define({{"geometry", {{"type", "Feature"}, {"geometry", square(35.1, 48.6, 0.05)}}}, {"name", "Field"}}).id(),
              a.id())
        << "a Feature of the same polygon";

    json renamed = field();
    renamed["name"] = "Meadow";
    EXPECT_NE(areas.define(renamed).id(), a.id()) << "the name is in the package";
    json fewer = field();
    fewer["layers"] = {"basemap"};
    EXPECT_NE(areas.define(fewer).id(), a.id());
    json unnamed = field();
    unnamed.erase("name");
    EXPECT_EQ(areas.define(unnamed).name(), RegionAreas::kDefaultName);
}

TEST_F(RegionAreasTest, RefusesWhatIsWrongOrOverTheLimits) {
    RegionAreas::Limits limits;
    limits.maxAreaKm2 = 100.0;
    limits.maxVertices = 20;
    limits.maxZoom = 13;
    const RegionAreas areas(settings(limits), log_, clock_);
    const auto refused = [&areas](const json& body, const std::string& says) {
        try {
            (void)areas.define(body);
            ADD_FAILURE() << "accepted " << body.dump();
        } catch (const AreaRefused& e) {
            EXPECT_NE(std::string(e.what()).find(says), std::string::npos) << e.what();
        }
    };
    refused(json::array(), "geometry");
    refused({{"name", "x"}}, "geometry");
    refused({{"geometry", square(35, 48, 0.5)}}, "more than the 100 km²");
    refused({{"geometry", ring(35, 48, 0.01, 30)}}, "30 vertices, more than the 20");
    // A bow tie measures small (its halves cancel) but has no clear inside.
    const json bowTie = {{"type", "Polygon"},
                         {"coordinates", {{{35.0, 48.0}, {35.5, 48.5}, {35.5, 48.0}, {35.0, 48.5}, {35.0, 48.0}}}}};
    refused({{"geometry", bowTie}}, "crosses itself");
    // A hole that is not in its polygon would be taken out of an area it is not part of.
    json outside = square(35, 48, 0.05);
    outside["coordinates"].push_back(square(35.2, 48, 0.05)["coordinates"][0]);
    refused({{"geometry", outside}}, "hole of the polygon is not inside");
    json across = square(35, 48, 0.05);
    across["coordinates"].push_back(square(35.04, 48.01, 0.02)["coordinates"][0]);
    refused({{"geometry", across}}, "hole of the polygon is not inside");
    json overlapping = square(35, 48, 0.05);
    overlapping["coordinates"].push_back(square(35.01, 48.01, 0.02)["coordinates"][0]);
    overlapping["coordinates"].push_back(square(35.02, 48.02, 0.02)["coordinates"][0]);
    refused({{"geometry", overlapping}}, "two holes");
    // A hole inside another would be taken out twice; either order.
    json nested = square(35, 48, 0.05);
    nested["coordinates"].push_back(square(35.005, 48.005, 0.04)["coordinates"][0]);
    nested["coordinates"].push_back(square(35.01, 48.01, 0.02)["coordinates"][0]);
    refused({{"geometry", nested}}, "two holes");
    std::swap(nested["coordinates"][1], nested["coordinates"][2]);
    refused({{"geometry", nested}}, "two holes");
    json holed = square(35, 48, 0.05);
    holed["coordinates"].push_back(square(35.01, 48.01, 0.02)["coordinates"][0]);
    EXPECT_NO_THROW((void)areas.define({{"geometry", holed}}));
    EXPECT_NO_THROW((void)areas.define({{"geometry", ring(35, 48, 0.01, 20)}}));
    refused({{"geometry", square(35, 48, 0.05)}, {"maxzoom", 14}}, "at most 13");
    refused({{"geometry", square(35, 48, 0.05)}, {"bbox", {35, 48, 36, 49}}}, "unknown field 'bbox'");
    refused({{"geometry", square(35, 48, 0.05)}, {"boundary", "/etc/passwd"}}, "unknown field 'boundary'");
    refused({{"geometry", {{"type", "Point"}, {"coordinates", {35, 48}}}}}, "no Polygon");
    refused({{"geometry", square(35, 48, 0.05)}, {"name", 7}}, "");
    refused({{"geometry", square(35, 48, 0.05)}, {"layers", {"roads"}}}, "unknown layer");
    EXPECT_NO_THROW((void)areas.define({{"geometry", square(35, 48, 0.05)}, {"maxzoom", 12}}));
}

TEST_F(RegionAreasTest, QueuesEachAreaOnceAndBuildsItInTurn) {
    RegionAreas areas(settings(), log_, clock_);
    const AreaStatus first = areas.request(field());
    EXPECT_EQ(first.state, AreaState::Queued);
    EXPECT_EQ(first.position, 1U);
    EXPECT_FALSE(first.package);
    EXPECT_EQ(areas.request(field()).position, 1U) << "asked twice, queued once";
    const AreaStatus second = areas.request(field(35.2, 48.6));
    EXPECT_EQ(second.position, 2U);
    EXPECT_EQ(areas.pending(), 2U);
    EXPECT_EQ(areas.status(second.id)->state, AreaState::Queued);
    EXPECT_EQ(extracts(), 0);

    areas.drain();
    EXPECT_EQ(areas.pending(), 0U);
    const auto built = areas.status(first.id);
    ASSERT_TRUE(built);
    EXPECT_EQ(built->state, AreaState::Ready);
    ASSERT_TRUE(built->package);
    EXPECT_EQ(built->package->name, "Field");
    EXPECT_EQ(built->package->maxZoom, 14);
    const test::ZipContents zip(areas.store().archive(*built->package));
    EXPECT_EQ(zip.files.at("tiles/basemap.pmtiles"), "PMTiles basemap");
    const int cuts = extracts();
    EXPECT_GT(cuts, 0);

    const AreaStatus again = areas.request(field());
    EXPECT_EQ(again.state, AreaState::Ready) << "a current package answers at once";
    EXPECT_EQ(again.package->version, built->package->version);
    EXPECT_EQ(extracts(), cuts);
    EXPECT_FALSE(areas.status("area-000000000000000000000000"));

    // Another server process (or a restart) finds the package on disk.
    const RegionAreas later(settings(), log_, clock_);
    EXPECT_EQ(later.status(first.id)->state, AreaState::Ready);
}

TEST_F(RegionAreasTest, NewWorldDataBuildsTheAreaAgainAndKeepsTheOldPackageMeanwhile) {
    RegionAreas areas(settings(), log_, clock_);
    const std::string id = areas.request(field()).id;
    areas.drain();
    const RegionPackage before = *areas.status(id)->package;
    world_->write("basemap.pmtiles", "PMTiles basemap, newer");
    const AreaStatus queued = areas.request(field());
    EXPECT_EQ(queued.state, AreaState::Queued);
    ASSERT_TRUE(queued.package);
    EXPECT_EQ(queued.package->version, before.version);
    areas.drain();
    EXPECT_NE(areas.status(id)->package->version, before.version);
}

TEST_F(RegionAreasTest, AFullQueueTurnsANewAreaAway) {
    RegionAreas::Limits limits;
    limits.maxQueued = 1;
    RegionAreas areas(settings(limits), log_, clock_);
    (void)areas.request(field());
    EXPECT_NO_THROW((void)areas.request(field())) << "the area already queued";
    EXPECT_THROW((void)areas.request(field(35.2, 48.6)), AreaQueueFull);
    areas.drain();
    EXPECT_EQ(areas.request(field(35.2, 48.6)).state, AreaState::Queued);
}

TEST_F(RegionAreasTest, AFailedBuildSaysWhyAndIsTriedAgainWhenAskedAgain) {
    RegionAreas areas(settings(), log_, clock_);
    fs::remove_all(dir_ / "world" / "fonts" / "Noto Sans Medium");
    const std::string id = areas.request(field()).id;
    areas.drain();
    const auto failed = areas.status(id);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->state, AreaState::Failed);
    EXPECT_NE(failed->error.find("Noto Sans Medium"), std::string::npos) << failed->error;

    world_->write("fonts/Noto Sans Medium/0-255.pbf", "medium latin");
    EXPECT_EQ(areas.request(field()).state, AreaState::Queued);
    areas.drain();
    EXPECT_EQ(areas.status(id)->state, AreaState::Ready);

    // A failure is forgotten after an hour; the package on disk answers then.
    clock_.advance(std::chrono::hours(2));
    EXPECT_EQ(areas.status(id)->state, AreaState::Ready);
}

TEST_F(RegionAreasTest, RemovesWhatNobodyUsedAndTheLeastRecentlyUsedOverTheLimit) {
    RegionAreas::Limits limits;
    limits.keepHours = 24;
    RegionAreas areas(settings(limits), log_, clock_);
    const std::string a = areas.request(field(35.1, 48.6)).id;
    areas.drain();
    clock_.advance(std::chrono::hours(1));
    const std::string b = areas.request(field(35.2, 48.6)).id;
    areas.drain();
    EXPECT_EQ(areas.sweep(), 0U);

    // Downloading a keeps it; b goes once it is a day old.
    clock_.advance(std::chrono::hours(20));
    ASSERT_TRUE(areas.use(a));
    clock_.advance(std::chrono::hours(5));
    EXPECT_GT(areas.sweep(), 0U);
    EXPECT_TRUE(areas.status(a));
    EXPECT_FALSE(areas.status(b));

    // Over the stored size the least recently used goes first.
    const std::uintmax_t size = areas.status(a)->package->size;
    limits.maxStoredBytes = size + size / 2;
    RegionAreas tight(settings(limits), log_, clock_);
    const std::string c = tight.request(field(35.3, 48.6)).id;
    tight.drain();
    EXPECT_GT(tight.sweep(), 0U);
    EXPECT_FALSE(tight.status(a));
    EXPECT_TRUE(tight.status(c));
}

TEST_F(RegionAreasTest, AreOffWithoutAWorldDirectoryOrAnAreaLimit) {
    RegionAreas::Limits off;
    off.maxAreaKm2 = 0.0;
    RegionAreas none(settings(off), log_, clock_);
    EXPECT_FALSE(none.enabled());
    EXPECT_THROW((void)none.request(field()), AreaRefused);
    EXPECT_EQ(none.sweep(), 0U);
    const RegionAreas noWorld({}, log_, clock_);
    EXPECT_FALSE(noWorld.enabled());
}

TEST_F(RegionAreasTest, TheApiDescribesAnAreaWithItsOwnUrls) {
    RegionAreas areas(settings(), log_, clock_);
    const AreaStatus queued = areas.request(field());
    const json q = RegionApi::describe(queued);
    EXPECT_EQ(q["state"], "queued");
    EXPECT_EQ(q["position"], 1);
    EXPECT_TRUE(q["package"].is_null());
    areas.drain();
    const json ready = RegionApi::describe(*areas.status(queued.id));
    EXPECT_EQ(ready["state"], "ready");
    EXPECT_TRUE(ready["position"].is_null());
    const std::string version = ready["package"]["version"];
    EXPECT_EQ(ready["package"]["package"], "/api/v1/areas/" + queued.id + "/packages/" + version + ".zip");
}
