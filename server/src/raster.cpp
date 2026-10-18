#include "raster.hpp"

#include <rfcore/geodesy.hpp>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace rfmap {
namespace fs = std::filesystem;

namespace {
std::atomic<std::uint64_t> nextRasterId{1};

std::uint64_t blockKey(std::uint64_t raster, int bx, int by) {
    return (raster << 44) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(bx)) << 22) ^
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(by));
}
}  // namespace

GdalRaster::GdalRaster(const fs::path& file, BlockCache& cache) : file_(file), cache_(cache), id_(nextRasterId++) {
    static std::once_flag once;
    std::call_once(once, [] {
        GDALAllRegister();
        // Our own LRU holds the decoded blocks; GDAL's cache would only hold them twice.
        GDALSetCacheMax64(16 << 20);
    });
    dataset_ = static_cast<GDALDataset*>(GDALOpen(file.c_str(), GA_ReadOnly));
    if (!dataset_) throw std::runtime_error("cannot open raster " + file.string());
    if (dataset_->GetRasterCount() < 1) throw std::runtime_error(file.string() + " has no bands");
    band_ = dataset_->GetRasterBand(1);
    width_ = dataset_->GetRasterXSize();
    height_ = dataset_->GetRasterYSize();
    band_->GetBlockSize(&blockW_, &blockH_);
    int hasNoData = 0;
    const double nd = band_->GetNoDataValue(&hasNoData);
    if (hasNoData) noData_ = nd;

    std::array<double, 6> gt{};
    if (dataset_->GetGeoTransform(gt.data()) != CE_None) throw std::runtime_error(file.string() + " has no geotransform");
    if (!GDALInvGeoTransform(gt.data(), inverse_.data())) {
        throw std::runtime_error(file.string() + ": geotransform not invertible");
    }

    const OGRSpatialReference* srs = dataset_->GetSpatialRef();
    if (!srs) throw std::runtime_error(file.string() + " has no coordinate reference system");
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target(*srs);
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (!target.IsSame(&wgs84)) {
        toRaster_ = OGRCreateCoordinateTransformation(&wgs84, &target);
        if (!toRaster_) throw std::runtime_error(file.string() + ": cannot transform from WGS84");
    }

    // Pixel centre positions back in WGS84, to measure the pixel pitch on the ellipsoid.
    std::unique_ptr<OGRCoordinateTransformation> toWgs(
        toRaster_ ? OGRCreateCoordinateTransformation(&target, &wgs84) : nullptr);
    auto centreOf = [&gt, &toWgs](double col, double row) {
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

    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    identity_ = fs::absolute(file).string() + ":" + std::to_string(size) + ":" + std::to_string(mtime);
}

GdalRaster::~GdalRaster() {
    if (toRaster_) OGRCoordinateTransformation::DestroyCT(toRaster_);
    if (dataset_) GDALClose(dataset_);
}

std::string GdalRaster::dataType() const { return GDALGetDataTypeName(band_->GetRasterDataType()); }

std::shared_ptr<const RasterBlock> GdalRaster::block(int bx, int by) const {
    const std::uint64_t key = blockKey(id_, bx, by);
    if (auto hit = cache_.get(key)) return hit;
    auto b = std::make_shared<RasterBlock>();
    b->width = std::min(blockW_, width_ - bx * blockW_);
    b->height = std::min(blockH_, height_ - by * blockH_);
    b->values.resize(static_cast<std::size_t>(b->width) * static_cast<std::size_t>(b->height));
    {
        std::lock_guard lock(ioMutex_);
        if (band_->RasterIO(GF_Read, bx * blockW_, by * blockH_, b->width, b->height, b->values.data(), b->width,
                            b->height, GDT_Float32, 0, 0) != CE_None) {
            throw std::runtime_error("read error in " + file_.string());
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
    if (!(colF >= 0.0 && rowF >= 0.0 && colF < width_ && rowF < height_)) return std::nullopt;
    const int col = static_cast<int>(colF);
    const int row = static_cast<int>(rowF);
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

std::string manifestSourceName(const fs::path& file) {
    const fs::path manifest = file.parent_path() / "manifest.json";
    std::ifstream in(manifest);
    if (!in) return {};
    try {
        const auto doc = nlohmann::json::parse(in);
        const std::string name = file.filename().string();
        // tiles-builder writes {"files": {"<name>": {"source": ..., ...}}} or a list with "name".
        if (doc.contains("files")) {
            const auto& files = doc.at("files");
            if (files.is_object() && files.contains(name)) return files.at(name).value("source", "");
            if (files.is_array()) {
                for (const auto& f : files) {
                    if (f.value("name", "") == name || f.value("file", "") == name) return f.value("source", "");
                }
            }
        }
    } catch (const nlohmann::json::exception&) {  // not JSON, or an entry of the wrong type
    }
    return {};
}

std::vector<std::string> manifestAttributions(const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file.parent_path() / "manifest.json");
    if (!in) return out;
    try {
        const auto doc = nlohmann::json::parse(in);
        if (doc.contains("files") && doc.at("files").is_array()) {
            for (const auto& f : doc.at("files")) {
                const std::string a = f.value("attribution", "");
                if (!a.empty() && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
            }
        }
        if (out.empty()) {
            const std::string joined = doc.value("attribution_display_text", "");
            if (!joined.empty()) out.push_back(joined);
        }
    } catch (const nlohmann::json::exception&) {  // not JSON, or an entry of the wrong type
    }
    return out;
}

std::optional<std::set<std::string, std::less<>>> manifestFiles(const fs::path& file) {
    std::ifstream in(file.parent_path() / "manifest.json");
    if (!in) return std::nullopt;
    try {
        const auto doc = nlohmann::json::parse(in);
        if (!doc.contains("files") || !doc.at("files").is_array()) return std::nullopt;
        std::set<std::string, std::less<>> out;
        for (const auto& f : doc.at("files")) {
            const std::string name = f.value("file", "");
            if (!name.empty()) out.insert(name);
        }
        return out;
    } catch (const nlohmann::json::exception&) {  // not JSON, or an entry that is not an object
        return std::nullopt;
    }
}

}  // namespace rfmap
