#include "region_api.hpp"

#include "security.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace rfmap {

namespace {

/// Downloads by version never change; the browser may keep them as long as it likes.
constexpr const char* kImmutable = "public, max-age=31536000, immutable";

/// How long a client waits before it asks for a drawn area again when the queue is full.
constexpr int kQueueFullRetryS = 60;

/// The download slots: as asked, or a quarter of the request threads but at least one. Nobody
/// queues for one; a client that finds them busy retries after the Retry-After.
Bulkhead::Limits downloadLimits(int maxDownloads, int serverThreads) {
    const int automatic = std::max(1, serverThreads / 4);
    return {maxDownloads > 0 ? maxDownloads : automatic, 0, std::chrono::milliseconds(0)};
}

}  // namespace

RegionApi::RegionApi(RegionCatalog& catalog, RegionAreas& areas, const AdminToken& admin, int maxDownloads,
                     int serverThreads)
    : catalog_(catalog),
      areas_(areas),
      admin_(admin),
      downloads_(std::make_unique<Bulkhead>(downloadLimits(maxDownloads, serverThreads),
                                            "too many package downloads at once, try again shortly",
                                            "regions.maxDownloads")) {}

json RegionApi::describe(const RegionPackage& p, std::string_view collection) {
    const std::string base = std::format("{}/{}/{}", kPrefix, collection, p.id);
    return {{"id", p.id},
            {"name", p.name},
            {"bbox", p.bbox.toJson()},
            {"maxzoom", p.maxZoom.has_value() ? json(*p.maxZoom) : json()},
            {"layers", p.layers},
            {"size", p.size},
            {"version", p.version},
            {"sha256", p.sha256},
            {"builtAt", p.builtAt},
            {"preview", p.preview ? json(std::format("{}/previews/{}.png", base, p.previewVersion)) : json()},
            {"package", std::format("{}/packages/{}.zip", base, p.version)}};
}

json RegionApi::describe(const AreaStatus& s) {
    return {{"id", s.id},
            {"state", areaStateName(s.state)},
            {"position", s.state == AreaState::Queued ? json(s.position) : json()},
            {"error", s.error.empty() ? json() : json(s.error)},
            {"package", s.package ? describe(*s.package, "areas") : json()}};
}

void RegionApi::sendTagged(const HttpRequest& req, HttpReply& res, const json& body) {
    const std::string text = body.dump();
    const std::string etag = "\"" + Secrets::hex(Secrets::sha256(text)) + "\"";
    // Revalidated every time: the answer is a few hundred bytes, or nothing with 304.
    res.header("Cache-Control", "no-cache");
    if (req.holdsCurrent(etag)) {
        res.notModified(etag);
        return;
    }
    res.header("ETag", etag);
    res.text(200, text, "application/json");
}

RegionPackage RegionApi::current(const HttpRequest& req) const {
    auto package = catalog_.find(req.match(1));
    if (!package) throw HttpError(404, "no such region");
    return *package;
}

RegionPackage RegionApi::exact(const HttpRequest& req) const {
    RegionPackage package = current(req);
    if (req.match(2) != package.version) {
        throw HttpError(404, "this version of the region was replaced", {{"version", package.version}});
    }
    return package;
}

void RegionApi::getList(const HttpRequest& req, HttpReply& res) const {
    json regions = json::array();
    for (const RegionPackage& p : catalog_.packages()) regions.push_back(describe(p));
    sendTagged(req, res, {{"regions", regions}});
}

void RegionApi::getRegion(const HttpRequest& req, HttpReply& res) const { sendTagged(req, res, describe(current(req))); }

void RegionApi::sendPackage(const HttpRequest& req, HttpReply& res, const RegionPackage& p,
                            const RegionPackageStore& store) const {
    res.header("Cache-Control", kImmutable);
    if (req.holdsCurrent(p.etag())) {
        res.notModified(p.etag());
        return;
    }
    res.header("ETag", p.etag());
    res.header("Content-Disposition", std::format("attachment; filename=\"{}-{}.zip\"", p.id, p.version.substr(0, 12)));
    // The slot is held by the response body and freed once it is sent or the client leaves.
    auto slot = std::make_shared<const Bulkhead::Permit>(downloads_->enter());
    res.file(store.archive(p), "application/zip", !req.header("Range").empty(), std::move(slot));
}

void RegionApi::sendPreview(const HttpRequest& req, HttpReply& res, const RegionPackage& p,
                            const RegionPackageStore& store) {
    if (!p.preview) throw HttpError(404, "this region has no preview");
    if (req.match(2) != p.previewVersion) throw HttpError(404, "this preview was replaced");
    res.header("Cache-Control", kImmutable);
    if (req.holdsCurrent(p.previewEtag())) {
        res.notModified(p.previewEtag());
        return;
    }
    res.header("ETag", p.previewEtag());
    res.file(store.preview(p), "image/png", !req.header("Range").empty());
}

void RegionApi::getPackage(const HttpRequest& req, HttpReply& res) const {
    sendPackage(req, res, exact(req), catalog_.store());
}

void RegionApi::getPreview(const HttpRequest& req, HttpReply& res) const {
    sendPreview(req, res, current(req), catalog_.store());
}

void RegionApi::getAreas(const HttpRequest& req, HttpReply& res) const {
    const RegionAreas::Limits& l = areas_.limits();
    sendTagged(req, res,
               {{"enabled", areas_.enabled()},
                {"maxAreaKm2", l.maxAreaKm2},
                {"maxVertices", l.maxVertices},
                {"maxzoom", l.maxZoom},
                {"layers", RegionDefinition::kLayers}});
}

void RegionApi::postArea(const HttpRequest& req, HttpReply& res) const {
    if (!areas_.enabled()) throw HttpError(409, "drawn areas are off on this server");
    try {
        const AreaStatus status = areas_.request(req.body());
        res.send(status.state == AreaState::Ready ? 200 : 202, describe(status));
    } catch (const AreaRefused& e) {
        throw HttpError(422, e.what());
    } catch (const AreaQueueFull& e) {
        throw HttpError(503, std::string(e.what()) + "; try again shortly", {{"retryAfterS", kQueueFullRetryS}});
    } catch (const RegionError& e) {  // the world data or the pmtiles tool
        throw HttpError(503, std::string("drawn areas cannot be built now: ") + e.what(), {{"retryAfterS", kQueueFullRetryS}});
    }
}

void RegionApi::getArea(const HttpRequest& req, HttpReply& res) const {
    const auto status = areas_.status(req.match(1));
    if (!status) throw HttpError(404, "no such area; ask for it again");
    res.header("Cache-Control", "no-store");
    res.send(200, describe(*status));
}

void RegionApi::getAreaPackage(const HttpRequest& req, HttpReply& res) const {
    const auto package = areas_.use(req.match(1));
    if (!package) throw HttpError(404, "no such area; ask for it again");
    if (req.match(2) != package->version) {
        throw HttpError(404, "this version of the area was replaced", {{"version", package->version}});
    }
    sendPackage(req, res, *package, areas_.store());
}

void RegionApi::getAreaPreview(const HttpRequest& req, HttpReply& res) const {
    const auto status = areas_.status(req.match(1));
    if (!status || !status->package) throw HttpError(404, "no such area; ask for it again");
    sendPreview(req, res, *status->package, areas_.store());
}

void RegionApi::getAdminList(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    json regions = json::array();
    for (const RegionStatus& s : catalog_.statuses()) {
        regions.push_back({{"id", s.id},
                           {"state", regionStateName(s.state)},
                           {"error", s.error.empty() ? json() : json(s.error)},
                           {"package", s.package ? describe(*s.package) : json()}});
    }
    res.send(200, {{"enabled", catalog_.enabled()},
                   {"scans", catalog_.scans()},
                   {"regions", regions},
                   {"areas", {{"enabled", areas_.enabled()}, {"pending", areas_.pending()}}}});
}

void RegionApi::postAdminReload(const HttpRequest& req, HttpReply& res) const {
    admin_.require(req);
    if (!catalog_.enabled()) throw HttpError(409, "offline regions are off: set regions.worldDir");
    catalog_.reload();
    res.send(202, {{"queued", true}});
}

}  // namespace rfmap
