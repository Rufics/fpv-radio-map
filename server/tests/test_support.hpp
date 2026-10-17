#pragma once

#include "common.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rfmap::test {

/// A directory for a test's files in the build tree (the tests run there), never in the shared,
/// world-writable temp directory.
inline std::filesystem::path scratchDir(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::current_path() / "test-scratch" / name;
    std::filesystem::create_directories(dir);
    return dir;
}

class FakeClock final : public IClock {
public:
    explicit FakeClock(TimePoint t = Clock::from_time_t(1'790'000'000)) : now_(t) {}
    [[nodiscard]] TimePoint now() const override { return now_; }
    void advance(std::chrono::seconds s) { now_ += s; }

private:
    TimePoint now_;
};

/// A small Float32 Cloud Optimized GeoTIFF in EPSG:4326, made like tools/tiles-builder makes
/// dem.cog.tif (NoData -32768). `height(col, row)` gives each pixel; return NAN for NoData.
inline void writeCog(const std::filesystem::path& out, double west, double north, double pixelDeg, int cols, int rows,
                     const std::function<double(int, int)>& height, GDALDataType type = GDT_Float32,
                     double noData = -32768.0) {
    GDALAllRegister();
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDriver* cog = GetGDALDriverManager()->GetDriverByName("COG");
    if (!mem || !cog) throw std::runtime_error("GDAL lacks the MEM or COG driver");
    GDALDataset* ds = mem->Create("", cols, rows, 1, type, nullptr);
    double gt[6] = {west, pixelDeg, 0.0, north, 0.0, -pixelDeg};
    ds->SetGeoTransform(gt);
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    ds->SetSpatialRef(&srs);
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->SetNoDataValue(noData);
    std::vector<float> row(static_cast<std::size_t>(cols));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const double h = height(c, r);
            row[static_cast<std::size_t>(c)] = static_cast<float>(std::isnan(h) ? noData : h);
        }
        if (band->RasterIO(GF_Write, 0, r, cols, 1, row.data(), cols, 1, GDT_Float32, 0, 0) != CE_None) {
            throw std::runtime_error("write failed");
        }
    }
    const char* opts[] = {"BLOCKSIZE=256", "COMPRESS=DEFLATE", nullptr};
    std::filesystem::remove(out);
    GDALDataset* outDs = cog->CreateCopy(out.c_str(), ds, FALSE, const_cast<char**>(opts), nullptr, nullptr);
    if (!outDs) throw std::runtime_error("COG CreateCopy failed");
    GDALClose(outDs);
    GDALClose(ds);
}

}  // namespace rfmap::test
