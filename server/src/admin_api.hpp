#pragma once

#include "request.hpp"
#include "services.hpp"

#include <functional>
#include <string>

namespace rfmap {

/// The admin API: module settings and reload, the trusted keys, caches, metrics and the
/// settings sync between server processes. Modules are installed and keys are trusted on the
/// machine with the rfmod CLI; this API only shows them and picks up the result. Every change
/// is announced to the other processes (SettingsSync).
class AdminApi {
public:
    using MetricsText = std::function<std::string()>;

    AdminApi(const Services& services, const AdminToken& admin, MetricsText metrics)
        : s_(services), admin_(admin), metrics_(std::move(metrics)) {}

    void putModule(const HttpRequest& req, HttpReply& res) const;
    void postModulesReload(const HttpRequest& req, HttpReply& res) const;
    void getNotices(const HttpRequest& req, HttpReply& res) const;
    void getKeys(const HttpRequest& req, HttpReply& res) const;
    void getCache(const HttpRequest& req, HttpReply& res) const;
    void putCache(const HttpRequest& req, HttpReply& res) const;
    void deleteCache(const HttpRequest& req, HttpReply& res) const;
    void getMetrics(const HttpRequest& req, HttpReply& res) const;
    void getSettingsSync(const HttpRequest& req, HttpReply& res) const;
    void postSettingsReload(const HttpRequest& req, HttpReply& res) const;

private:
    /// Applies {enabled?, options?} to an installed module; HttpError 422 when refused.
    void updateInstalled(const std::string& id, const json& body) const;
    [[nodiscard]] json trustedKeys() const;
    /// Tells the other processes that the module files changed; HttpError 503 when it cannot,
    /// since no poll covers files and they would keep the old modules until a reset.
    void announceModules() const;

    const Services& s_;
    const AdminToken& admin_;
    MetricsText metrics_;
};

}  // namespace rfmap
