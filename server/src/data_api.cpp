#include "data_api.hpp"

namespace rfmap {

// ---- library ----

std::string DataApi::libraryKind(const std::string& kind) const {
    (void)SchemaRegistry::libraryFile(kind);
    if (kind == "radio-profiles" && !s_.builtins().enabled(BuiltinModules::kRadioProfiles)) {
        throw HttpError(404, "radio profiles (rf.radio-profiles) are disabled on this server");
    }
    return kind;
}

void DataApi::getLibraryList(const HttpRequest& req, HttpReply& res) const {
    res.send(200, s_.library().list(libraryKind(req.match(1))));
}

void DataApi::getLibraryItem(const HttpRequest& req, HttpReply& res) const {
    const auto item = s_.library().get(libraryKind(req.match(1)), req.match(2));
    if (!item) throw HttpError(404, "no such library item");
    res.send(200, *item);
}

void DataApi::getLibraryHistory(const HttpRequest& req, HttpReply& res) const {
    res.send(200, s_.library().history(libraryKind(req.match(1)), req.match(2)));
}

void DataApi::postLibraryItem(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(201, s_.library().add(libraryKind(req.match(1)), req.body(), "admin@" + req.client()));
}

void DataApi::putLibraryItem(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    res.send(200, s_.library().update(libraryKind(req.match(1)), req.match(2), req.body(), "admin@" + req.client()));
}

// ---- weather ----

void DataApi::getWeatherPoint(const HttpRequest& req, HttpReply& res) const {
    const double lat = req.number("lat");
    const double lon = req.number("lon");
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) throw HttpError(400, "lat/lon out of range");
    const TimePoint t = req.time("timeUtc").value_or(Clock::now());
    const ForecastAt f = s_.weather().at(lat, lon, t);
    json out = f.point;
    out["notes"] = json::array();
    for (const auto& n : f.notes) out["notes"].push_back({{"what", n.what}, {"detail", n.detail}});
    res.send(200, out);
}

void DataApi::getWeatherHorizon(const HttpRequest&, HttpReply& res) const {
    const rfcore::GeoPoint probe = s_.dem() ? s_.dem()->raster().centre() : rfcore::GeoPoint{0.0, 0.0};
    res.send(200, s_.weather().horizon(probe.latitudeDeg, probe.longitudeDeg));
}

void DataApi::getMapConfig(const HttpRequest&, HttpReply& res) const {
    res.send(200, MapConfig(s_.config(), s_.dem()).build());
}

// ---- modules ----

void DataApi::getModules(const HttpRequest& req, HttpReply& res) const {
    res.send(200, s_.catalog().list(admin_.presentedBy(req)));
}

bool DataApi::mayReadAlpha(const HttpRequest& req, const std::string& moduleId) const {
    // An absent grant reads as empty, which is never valid.
    return admin_.presentedBy(req) || s_.catalog().grantValid(moduleId, req.param("grant"), Clock::now());
}

void DataApi::getModuleFile(const HttpRequest& req, HttpReply& res) const {
    const std::string id = req.match(1);
    auto file = s_.catalog().file(id, req.match(2));
    if (!file.has_value() || (file->alpha && !mayReadAlpha(req, id))) throw HttpError(404, "no such module file");
    // The bytes came with the lookup, from the version the alpha flag was decided for.
    // A module file is a resource the app imports, never a page: no sniffing, no scripts if a
    // browser is pointed at it directly, no use from another origin.
    res.header("X-Content-Type-Options", "nosniff");
    res.header("Content-Security-Policy", "default-src 'none'; sandbox");
    res.header("Cross-Origin-Resource-Policy", "same-origin");
    res.header("Cache-Control", file->alpha ? "private, no-store" : "public, max-age=300");
    res.text(200, file->content, file->contentType.c_str());
}

}  // namespace rfmap
