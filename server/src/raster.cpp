#include "raster.hpp"

#include <rfcore/geodesy.hpp>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace rfmap {
namespace fs = std::filesystem;

namespace {
/// A process-wide id per opened raster, so block cache keys of different rasters never meet.
std::uint64_t nextRasterId() {
    static std::atomic<std::uint64_t> next{1};
    return next++;
}

/// Block indices get 22 bits each in the cache key; the raster id takes the remaining 20.
constexpr int kBlockIndexBits = 22;
constexpr std::uint64_t kBlockIndexMask = (std::uint64_t{1} << kBlockIndexBits) - 1;
}  // namespace

void GdalRaster::DatasetCloser::operator()(GDALDataset* dataset) const { GDALClose(dataset); }

void GdalRaster::TransformationDestroyer::operator()(OGRCoordinateTransformation* transformation) const {
    OGRCoordinateTransformation::DestroyCT(transformation);
}

GdalRaster::GdalRaster(const fs::path& file, BlockCache& cache) : file_(file), cache_(cache), id_(nextRasterId()) {
    static std::once_flag once;
    std::call_once(once, [] {
        GDALAllRegister();
        // Our own LRU holds the decoded blocks; GDAL's cache would only hold them twice.
        GDALSetCacheMax64(16 << 20);
    });
    open();
    std::array<double, 6> gt{};
    if (dataset_->GetGeoTransform(gt.data()) != CE_None) throw RasterError(file.string() + " has no geotransform");
    if (!GDALInvGeoTransform(gt.data(), inverse_.data())) {
        throw RasterError(file.string() + ": geotransform not invertible");
    }
    readProjection(gt);

    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    identity_ = std::format("{}:{}:{}", fs::absolute(file).string(), size, mtime);
}

GdalRaster::~GdalRaster() = default;

void GdalRaster::open() {
    dataset_.reset(static_cast<GDALDataset*>(GDALOpen(file_.c_str(), GA_ReadOnly)));
    if (!dataset_) throw RasterError("cannot open raster " + file_.string());
    if (dataset_->GetRasterCount() < 1) throw RasterError(file_.string() + " has no bands");
    band_ = dataset_->GetRasterBand(1);
    width_ = dataset_->GetRasterXSize();
    height_ = dataset_->GetRasterYSize();
    band_->GetBlockSize(&blockW_, &blockH_);
    if (blockW_ < 1 || blockH_ < 1) throw RasterError(file_.string() + " has an empty block size");
    // Each block index must fit its part of the cache key.
    const std::uint64_t blocksAcross = static_cast<std::uint64_t>(width_ / blockW_) + 1;
    const std::uint64_t blocksDown = static_cast<std::uint64_t>(height_ / blockH_) + 1;
    if (blocksAcross > kBlockIndexMask || blocksDown > kBlockIndexMask) {
        throw RasterError(file_.string() + " has more blocks than the block cache can address");
    }
    int hasNoData = 0;
    const double nd = band_->GetNoDataValue(&hasNoData);
    if (hasNoData) noData_ = nd;
}

void GdalRaster::readProjection(const std::array<double, 6>& geoTransform) {
    const OGRSpatialReference* srs = dataset_->GetSpatialRef();
    if (!srs) throw RasterError(file_.string() + " has no coordinate reference system");
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target(*srs);
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    Transformation toWgs;
    if (!target.IsSame(&wgs84)) {
        toRaster_.reset(OGRCreateCoordinateTransformation(&wgs84, &target));
        if (!toRaster_) throw RasterError(file_.string() + ": cannot transform from WGS84");
        toWgs.reset(OGRCreateCoordinateTransformation(&target, &wgs84));
        if (!toWgs) throw RasterError(file_.string() + ": cannot transform to WGS84");
    }
    measurePitch(geoTransform, toWgs.get());
}

void GdalRaster::measurePitch(const std::array<double, 6>& gt, OGRCoordinateTransformation* toWgs) {
    // Pixel centre positions back in WGS84, to measure the pixel pitch on the ellipsoid.
    auto centreOf = [&gt, toWgs](double col, double row) {
        double x = gt[0] + (col + 0.5) * gt[1] + (row + 0.5) * gt[2];
        double y = gt[3] + (col + 0.5) * gt[4] + (row + 0.5) * gt[5];
        if (toWgs) toWgs->Transform(1, &x, &y);
        return rfcore::GeoPoint{y, x};
    };
    const rfcore::Geodesy geo;
    double best = std::numeric_limits<double>::infinity();
    // East-west pitch shrinks towards the poles, so check the top, middle and bottom rows.
    for (const double row : {0.0, std::floor(height_ / 2.0), static_cast<double>(height_ - 2)}) {
        for (const double col : {0.0, std::floor(width_ / 2.0), static_cast<double>(width_ - 2)}) {
            if (row < 0 || col < 0) continue;
            const auto p = centreOf(col, row);
            best = std::min(best, geo.inverse(p, centreOf(col + 1, row)).distance.value());
            best = std::min(best, geo.inverse(p, centreOf(col, row + 1)).distance.value());
        }
    }
    resolution_ = rfcore::Metres(best);
    centre_ = centreOf(width_ / 2.0 - 0.5, height_ / 2.0 - 0.5);
}

std::uint64_t GdalRaster::blockKey(int bx, int by) const {
    return (id_ << (2 * kBlockIndexBits)) | ((static_cast<std::uint64_t>(bx) & kBlockIndexMask) << kBlockIndexBits) |
           (static_cast<std::uint64_t>(by) & kBlockIndexMask);
}

std::string GdalRaster::dataType() const { return GDALGetDataTypeName(band_->GetRasterDataType()); }

std::shared_ptr<const RasterBlock> GdalRaster::block(int bx, int by) const {
    const std::uint64_t key = blockKey(bx, by);
    if (auto hit = cache_.get(key)) return hit;
    auto b = std::make_shared<RasterBlock>();
    b->width = std::min(blockW_, width_ - bx * blockW_);
    b->height = std::min(blockH_, height_ - by * blockH_);
    b->values.resize(static_cast<std::size_t>(b->width) * static_cast<std::size_t>(b->height));
    {
        std::lock_guard lock(ioMutex_);
        if (band_->RasterIO(GF_Read, bx * blockW_, by * blockH_, b->width, b->height, b->values.data(), b->width,
                            b->height, GDT_Float32, 0, 0) != CE_None) {
            throw RasterError("read error in " + file_.string());
        }
    }
    cache_.put(key, b);
    return b;
}

std::optional<double> GdalRaster::valueAt(const rfcore::GeoPoint& p) const {
    double x = p.longitudeDeg;
    double y = p.latitudeDeg;
    if (toRaster_) {
        std::lock_guard lock(ioMutex_);
        if (!toRaster_->Transform(1, &x, &y)) return std::nullopt;
    }
    const double colF = inverse_[0] + x * inverse_[1] + y * inverse_[2];
    const double rowF = inverse_[3] + x * inverse_[4] + y * inverse_[5];
    // NaN fails this test too, so the casts below are always within the raster.
    if (!(colF >= 0.0 && rowF >= 0.0 && colF < width_ && rowF < height_)) return std::nullopt;
    const auto col = static_cast<int>(colF);
    const auto row = static_cast<int>(rowF);
    const auto b = block(col / blockW_, row / blockH_);
    const float v = b->values[static_cast<std::size_t>(row % blockH_) * static_cast<std::size_t>(b->width) +
                              static_cast<std::size_t>(col % blockW_)];
    if (std::isnan(v)) return std::nullopt;
    if (noData_.has_value() && static_cast<double>(v) == static_cast<double>(static_cast<float>(*noData_))) return std::nullopt;
    return static_cast<double>(v);
}

DemSource::DemSource(std::shared_ptr<GdalRaster> raster, std::string name, std::optional<double> statedErrorM,
                     std::string statedErrorSource)
    : raster_(std::move(raster)),
      name_(std::move(name)),
      statedError_(statedErrorM),
      statedErrorSource_(std::move(statedErrorSource)) {}

std::optional<rfcore::ElevationSample> DemSource::elevation(const rfcore::GeoPoint& point) const {
    const auto v = raster_->valueAt(point);
    if (!v.has_value()) return std::nullopt;
    return rfcore::ElevationSample{rfcore::Metres(*v), rfcore::Metres(statedError_.value_or(0.0))};
}

LandCoverSource::LandCoverSource(std::shared_ptr<GdalRaster> raster, std::string name)
    : raster_(std::move(raster)), name_(std::move(name)) {}

std::optional<int> LandCoverSource::code(const rfcore::GeoPoint& p) const {
    const auto v = raster_->valueAt(p);
    if (!v.has_value() || *v == 0.0) return std::nullopt;  // 0 is WorldCover "no data"
    return static_cast<int>(*v);
}

}  // namespace rfmap
