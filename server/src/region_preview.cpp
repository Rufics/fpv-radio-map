#include "region_package.hpp"

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <ogr_srs_api.h>

#include <memory>
#include <mutex>
#include <string>

namespace rfmap {

namespace {

/// A GDAL dataset handle, closed when it goes.
struct DatasetClose {
    void operator()(void* dataset) const { GDALClose(dataset); }
};
using Dataset = std::unique_ptr<void, DatasetClose>;

/// Arguments of a GDAL utility, as the C API takes them.
class GdalArgs {
public:
    explicit GdalArgs(std::initializer_list<std::string> args) {
        for (const std::string& a : args) list_.AddString(a.c_str());
    }
    void add(const std::string& arg) { list_.AddString(arg.c_str()); }
    [[nodiscard]] char** list() { return list_.List(); }

private:
    CPLStringList list_;
};

/// The world DEM over `box`, scaled to the preview width, in memory.
Dataset shrink(GDALDatasetH source, const GeoBox& box) {
    GdalArgs args{"-of", "MEM", "-outsize", std::to_string(RegionPreview::kWidth), "0", "-r", "average",
                  "-projwin_srs", "EPSG:4326", "-projwin", std::to_string(box.minLon()), std::to_string(box.maxLat()),
                  std::to_string(box.maxLon()), std::to_string(box.minLat())};
    const std::unique_ptr<GDALTranslateOptions, decltype(&GDALTranslateOptionsFree)> options(
        GDALTranslateOptionsNew(args.list(), nullptr), &GDALTranslateOptionsFree);
    int failed = 0;
    Dataset small(GDALTranslate("", source, options.get(), &failed));
    if (!small || failed) throw RegionError("the DEM does not cover the region");
    return small;
}

}  // namespace

void RegionPreview::render(const std::filesystem::path& dem, const GeoBox& box, const std::filesystem::path& png) {
    static std::once_flag once;
    std::call_once(once, [] { GDALAllRegister(); });
    // No .aux.xml next to the PNG: the preview is a picture, not a dataset.
    CPLSetThreadLocalConfigOption("GDAL_PAM_ENABLED", "NO");
    const Dataset source(GDALOpen(dem.string().c_str(), GA_ReadOnly));
    if (!source) throw RegionError("cannot open " + dem.string());
    const Dataset small = shrink(source.get(), box);

    GdalArgs args{"-of", "PNG", "-compute_edges"};
    OGRSpatialReferenceH srs = GDALGetSpatialRef(source.get());
    if (srs && OSRIsGeographic(srs)) {
        args.add("-s");
        args.add("111120");  // metres per degree: heights in metres over a grid in degrees
    }
    const std::unique_ptr<GDALDEMProcessingOptions, decltype(&GDALDEMProcessingOptionsFree)> options(
        GDALDEMProcessingOptionsNew(args.list(), nullptr), &GDALDEMProcessingOptionsFree);
    int failed = 0;
    const Dataset out(GDALDEMProcessing(png.string().c_str(), small.get(), "hillshade", nullptr, options.get(), &failed));
    if (!out || failed) throw RegionError("cannot draw the preview from " + dem.string());
}

}  // namespace rfmap
