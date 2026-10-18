// Raster files the server refuses, land cover codes, and the tiles-builder manifest next to the data.

#include "raster.hpp"
#include "test_support.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace rfmap;
namespace fs = std::filesystem;

namespace {

/// A 4x4 Float32 GeoTIFF of `value`, with or without a geotransform and a coordinate system.
class TiffBuilder {
public:
    explicit TiffBuilder(fs::path file) : file_(std::move(file)) { GDALAllRegister(); }

    TiffBuilder& geoTransform(const std::vector<double>& gt) {
        gt_ = gt;
        return *this;
    }
    TiffBuilder& epsg(int code) {
        epsg_ = code;
        return *this;
    }
    TiffBuilder& value(float v) {
        value_ = v;
        return *this;
    }

    const fs::path& write() const {
        GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
        GDALDataset* ds = gtiff->Create(file_.c_str(), 4, 4, 1, GDT_Float32, nullptr);
        if (!gt_.empty()) ds->SetGeoTransform(const_cast<double*>(gt_.data()));
        if (epsg_ != 0) {
            OGRSpatialReference srs;
            srs.importFromEPSG(epsg_);
            ds->SetSpatialRef(&srs);
        }
        std::vector<float> pixels(16, value_);
        (void)ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4, pixels.data(), 4, 4, GDT_Float32, 0, 0);
        GDALClose(ds);
        return file_;
    }

private:
    fs::path file_;
    std::vector<double> gt_;
    int epsg_ = 0;
    float value_ = 0.0f;
};

class RasterTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = test::scratchDir("raster") / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }

    /// Writes manifest.json next to the data files with `text` as its content.
    void manifest(const std::string& text) const { std::ofstream(dir_ / "manifest.json") << text; }

    [[nodiscard]] std::string refusal(const fs::path& file) {
        try {
            GdalRaster raster(file, cache_);
        } catch (const RasterError& e) {
            return e.what();
        }
        return "accepted";
    }

    fs::path dir_;
    BlockCache cache_{1u << 20, [](const RasterBlock& b) { return b.values.size() * sizeof(float); }};
};

}  // namespace

TEST_F(RasterTest, RefusesFilesItCannotPlaceOnTheEllipsoid) {
    EXPECT_NE(refusal(dir_ / "absent.tif").find("cannot open raster"), std::string::npos);
    EXPECT_NE(refusal(TiffBuilder(dir_ / "no-gt.tif").epsg(4326).write()).find("has no geotransform"), std::string::npos);
    EXPECT_NE(refusal(TiffBuilder(dir_ / "flat.tif").geoTransform({35.0, 0.0, 0.0, 48.0, 0.0, 0.0}).epsg(4326).write())
                  .find("not invertible"),
              std::string::npos);
    EXPECT_NE(refusal(TiffBuilder(dir_ / "no-crs.tif").geoTransform({35.0, 0.01, 0.0, 48.0, 0.0, -0.01}).write())
                  .find("no coordinate reference system"),
              std::string::npos);
}

TEST_F(RasterTest, ReadsAProjectedRasterThroughATransform) {
    // Web Mercator metres around (lon 35, lat 48): x = 3896182, y = 6106855.
    const fs::path file =
        TiffBuilder(dir_ / "mercator.tif").geoTransform({3896000.0, 100.0, 0.0, 6107000.0, 0.0, -100.0}).epsg(3857).value(42.0f).write();
    GdalRaster raster(file, cache_);
    EXPECT_GT(raster.nativeResolution().value(), 0.0);
    const auto v = raster.valueAt({48.0, 35.0});
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 42.0);
    EXPECT_FALSE(raster.valueAt({10.0, 10.0}).has_value());  // outside the raster
}

TEST_F(RasterTest, LandCoverZeroIsNoData) {
    const auto trees = std::make_shared<GdalRaster>(
        TiffBuilder(dir_ / "trees.tif").geoTransform({35.0, 0.01, 0.0, 48.0, 0.0, -0.01}).epsg(4326).value(10.0f).write(), cache_);
    const auto none = std::make_shared<GdalRaster>(
        TiffBuilder(dir_ / "none.tif").geoTransform({35.0, 0.01, 0.0, 48.0, 0.0, -0.01}).epsg(4326).value(0.0f).write(), cache_);
    EXPECT_EQ(LandCoverSource(trees, "trees").code({47.985, 35.015}), 10);
    EXPECT_FALSE(LandCoverSource(none, "none").code({47.985, 35.015}).has_value());
}

TEST_F(RasterTest, ManifestNamesTheSourceOfAFile) {
    const fs::path dem = dir_ / "dem.cog.tif";
    EXPECT_EQ(manifestSourceName(dem), "");  // no manifest
    manifest(R"({"files": {"dem.cog.tif": {"source": "Copernicus GLO-30"}}})");
    EXPECT_EQ(manifestSourceName(dem), "Copernicus GLO-30");
    manifest(R"({"files": [{"name": "other.tif", "source": "x"}, {"file": "dem.cog.tif", "source": "FABDEM"}]})");
    EXPECT_EQ(manifestSourceName(dem), "FABDEM");
    manifest(R"({"files": [{"name": "other.tif", "source": "x"}]})");
    EXPECT_EQ(manifestSourceName(dem), "");
    manifest(R"({"files": "dem.cog.tif"})");
    EXPECT_EQ(manifestSourceName(dem), "");
    manifest(R"({"files": [42]})");
    EXPECT_EQ(manifestSourceName(dem), "");  // an entry of the wrong type
    manifest("not json");
    EXPECT_EQ(manifestSourceName(dem), "");
    manifest(R"({"version": 1})");
    EXPECT_EQ(manifestSourceName(dem), "");
}

TEST_F(RasterTest, ManifestAttributionsAndFiles) {
    const fs::path tiles = dir_ / "basemap.pmtiles";
    EXPECT_TRUE(manifestAttributions(tiles).empty());
    EXPECT_FALSE(manifestFiles(tiles).has_value());

    manifest(R"({"files": [{"file": "a.pmtiles", "attribution": "A"}, {"file": "b.pmtiles", "attribution": "A"},
                           {"file": "c.pmtiles"}, {"attribution": "B"}]})");
    EXPECT_EQ(manifestAttributions(tiles), (std::vector<std::string>{"A", "B"}));
    const auto files = manifestFiles(tiles);
    ASSERT_TRUE(files.has_value());
    EXPECT_EQ(files->size(), 3u);

    manifest(R"({"files": [{"file": "a.pmtiles"}], "attribution_display_text": "All sources"})");
    EXPECT_EQ(manifestAttributions(tiles), (std::vector<std::string>{"All sources"}));
    manifest(R"({"attribution_display_text": ""})");
    EXPECT_TRUE(manifestAttributions(tiles).empty());
    EXPECT_FALSE(manifestFiles(tiles).has_value());
    manifest(R"({"files": ["a.pmtiles"]})");
    EXPECT_TRUE(manifestAttributions(tiles).empty());
    EXPECT_FALSE(manifestFiles(tiles).has_value());
}
