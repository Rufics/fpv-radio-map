#pragma once

#include "bulkhead.hpp"
#include "region_catalog.hpp"
#include "request.hpp"

#include <memory>
#include <string>

namespace rfmap {

/// Offline regions over HTTP: the catalog a client shows ("what to download", "is there an
/// update"), the packages and their previews, and the admin's view and reload.
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
    RegionApi(RegionCatalog& catalog, const AdminToken& admin, int maxDownloads, int serverThreads);

    void getList(const HttpRequest& req, HttpReply& res) const;
    void getRegion(const HttpRequest& req, HttpReply& res) const;
    void getPackage(const HttpRequest& req, HttpReply& res) const;
    void getPreview(const HttpRequest& req, HttpReply& res) const;
    void getAdminList(const HttpRequest& req, HttpReply& res) const;
    void postAdminReload(const HttpRequest& req, HttpReply& res) const;

    /// The gate of package downloads, for the metrics.
    [[nodiscard]] const Bulkhead& downloadGate() const { return *downloads_; }

    /// A package as the catalog lists it.
    [[nodiscard]] static json describe(const RegionPackage& package);

private:
    /// The current package of the region in capture 1; HttpError 404 without one.
    [[nodiscard]] RegionPackage current(const HttpRequest& req) const;
    /// The current package when capture 2 names its version; 404 (with the current version) otherwise.
    [[nodiscard]] RegionPackage exact(const HttpRequest& req) const;
    /// Sends `body` with an entity tag of its own, or 304 when the client has it.
    static void sendTagged(const HttpRequest& req, HttpReply& res, const json& body);

    RegionCatalog& catalog_;
    const AdminToken& admin_;
    std::unique_ptr<Bulkhead> downloads_;
};

}  // namespace rfmap
