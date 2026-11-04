#pragma once

#include "bulkhead.hpp"
#include "region_areas.hpp"
#include "region_catalog.hpp"
#include "request.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace rfmap {

/// Offline regions over HTTP: the catalog a client shows ("what to download", "is there an
/// update"), the packages and their previews, the areas users draw, and the admin's view and
/// reload.
///
/// Package and preview URLs carry the version, so each URL always names the same bytes: they
/// are cached for good, a Range request can resume a download safely, and an old URL answers
/// 404 with the current version once a newer package replaced it.
class RegionApi {
public:
    static constexpr const char* kPrefix = "/api/v1";

    /// `maxDownloads` packages stream at once (regions.maxDownloads, 0 = a quarter of the
    /// `serverThreads`); a slow client holds a request thread for its whole download, so more
    /// are turned away with 503 and Retry-After instead of starving the other requests.
    RegionApi(RegionCatalog& catalog, RegionAreas& areas, const AdminToken& admin, int maxDownloads, int serverThreads);

    void getList(const HttpRequest& req, HttpReply& res) const;
    void getRegion(const HttpRequest& req, HttpReply& res) const;
    void getPackage(const HttpRequest& req, HttpReply& res) const;
    void getPreview(const HttpRequest& req, HttpReply& res) const;
    /// GET /areas: whether areas can be drawn here, and their limits.
    void getAreas(const HttpRequest& req, HttpReply& res) const;
    /// POST /areas: 200 with the package when it is current, else 202 with the build queued.
    void postArea(const HttpRequest& req, HttpReply& res) const;
    void getArea(const HttpRequest& req, HttpReply& res) const;
    void getAreaPackage(const HttpRequest& req, HttpReply& res) const;
    void getAreaPreview(const HttpRequest& req, HttpReply& res) const;
    void getAdminList(const HttpRequest& req, HttpReply& res) const;
    void postAdminReload(const HttpRequest& req, HttpReply& res) const;

    /// The gate of package downloads, for the metrics.
    [[nodiscard]] const Bulkhead& downloadGate() const { return *downloads_; }

    /// A package as the catalog lists it; `collection` is "regions" or "areas", its URLs' path.
    [[nodiscard]] static json describe(const RegionPackage& package, std::string_view collection = "regions");
    /// The status of a drawn area as the API answers it.
    [[nodiscard]] static json describe(const AreaStatus& status);

private:
    /// The current package of the region in capture 1; HttpError 404 without one.
    [[nodiscard]] RegionPackage current(const HttpRequest& req) const;
    /// The current package when capture 2 names its version; 404 (with the current version) otherwise.
    [[nodiscard]] RegionPackage exact(const HttpRequest& req) const;
    /// Sends `body` with an entity tag of its own, or 304 when the client has it.
    static void sendTagged(const HttpRequest& req, HttpReply& res, const json& body);
    /// Streams the zip of `p` from `store` under a download slot.
    void sendPackage(const HttpRequest& req, HttpReply& res, const RegionPackage& p, const RegionPackageStore& store) const;
    /// Sends the preview of `p` when capture 2 names its version.
    static void sendPreview(const HttpRequest& req, HttpReply& res, const RegionPackage& p, const RegionPackageStore& store);

    RegionCatalog& catalog_;
    RegionAreas& areas_;
    const AdminToken& admin_;
    std::unique_ptr<Bulkhead> downloads_;
};

}  // namespace rfmap
