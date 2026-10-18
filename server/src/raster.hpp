#pragma once

#include "lru.hpp"

#include <rfcore/terrain_profile.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

class GDALDataset;
class GDALRasterBand;
class OGRCoordinateTransformation;

namespace rfmap {

/// A raster file that cannot be opened, read or placed on the WGS84 ellipsoid.
class RasterError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One decoded block of a raster band, as 32-bit floats (DEM heights and land cover codes both
/// fit exactly).
struct RasterBlock {
    int width = 0;
    int height = 0;
    std::vector<float> values;
};

using BlockCache = LruCache<std::uint64_t, RasterBlock>;

/// A single-band GeoTIFF/COG read block by block through a shared LRU. Values are the file's
/// own: the pixel containing the point is returned, never an interpolation, so every height
/// in a profile is a number the data provider published. NoData gives nothing.
class GdalRaster {
public:
    GdalRaster(const std::filesystem::path& file, BlockCache& cache);
    ~GdalRaster();
    GdalRaster(const GdalRaster&) = delete;
    GdalRaster& operator=(const GdalRaster&) = delete;

    [[nodiscard]] std::optional<double> valueAt(const rfcore::GeoPoint& p) const;
    /// Smallest ground distance between neighbouring pixel centres over the raster's extent.
    [[nodiscard]] rfcore::Metres nativeResolution() const { return resolution_; }
    [[nodiscard]] rfcore::GeoPoint centre() const { return centre_; }
    /// Path, size and modification time: changes when the operator replaces the file.
    [[nodiscard]] const std::string& identity() const { return identity_; }
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] std::optional<double> noData() const { return noData_; }
    [[nodiscard]] std::string dataType() const;

private:
    std::shared_ptr<const RasterBlock> block(int bx, int by) const;

    std::filesystem::path file_;
    BlockCache& cache_;
    std::uint64_t id_;
    GDALDataset* dataset_ = nullptr;
    GDALRasterBand* band_ = nullptr;
    OGRCoordinateTransformation* toRaster_ = nullptr;  ///< null when the raster is in WGS84 lon/lat
    std::array<double, 6> inverse_{};
    int width_ = 0, height_ = 0, blockW_ = 0, blockH_ = 0;
    std::optional<double> noData_;
    rfcore::Metres resolution_{0.0};
    rfcore::GeoPoint centre_{};
    std::string identity_;
    mutable std::mutex ioMutex_;  ///< GDAL datasets are not thread-safe
};

/// The calculation DEM (tools/tiles-builder dem.cog.tif: Float32, NoData -32768, EGM2008 heights).
class DemSource final : public rfcore::IElevationSource {
public:
    DemSource(std::shared_ptr<GdalRaster> raster, std::string name, std::optional<double> statedErrorM,
              std::string statedErrorSource);

    [[nodiscard]] std::optional<rfcore::ElevationSample> elevation(const rfcore::GeoPoint& point) const override;
    [[nodiscard]] rfcore::Metres nativeResolution() const override { return raster_->nativeResolution(); }
    [[nodiscard]] std::string name() const override { return name_; }

    [[nodiscard]] const GdalRaster& raster() const { return *raster_; }
    [[nodiscard]] std::optional<double> statedErrorM() const { return statedError_; }
    [[nodiscard]] const std::string& statedErrorSource() const { return statedErrorSource_; }

private:
    std::shared_ptr<GdalRaster> raster_;
    std::string name_;
    std::optional<double> statedError_;
    std::string statedErrorSource_;
};

/// ESA WorldCover classes (tools/tiles-builder landcover.cog.tif, NoData 0).
class LandCoverSource {
public:
    LandCoverSource(std::shared_ptr<GdalRaster> raster, std::string name);
    [[nodiscard]] std::optional<int> code(const rfcore::GeoPoint& p) const;
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const GdalRaster& raster() const { return *raster_; }

private:
    std::shared_ptr<GdalRaster> raster_;
    std::string name_;
};

/// Name of a tiles-builder output from the manifest.json next to it, if there is one.
std::string manifestSourceName(const std::filesystem::path& file);
/// Distinct per-file attributions from the tiles-builder manifest next to `file`; empty without one.
std::vector<std::string> manifestAttributions(const std::filesystem::path& file);
/// File names the tiles-builder manifest next to `file` lists; nullopt without a readable manifest.
std::optional<std::set<std::string, std::less<>>> manifestFiles(const std::filesystem::path& file);

}  // namespace rfmap
