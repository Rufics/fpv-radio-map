#pragma once

#include "map_config.hpp"
#include "request.hpp"
#include "services.hpp"

namespace rfmap {

/// The library, weather, map configuration and the public module list and files.
class DataApi {
public:
    DataApi(const Services& services, const AdminToken& admin) : s_(services), admin_(admin) {}

    void getLibraryList(const HttpRequest& req, HttpReply& res) const;
    void getLibraryItem(const HttpRequest& req, HttpReply& res) const;
    void getLibraryHistory(const HttpRequest& req, HttpReply& res) const;
    void postLibraryItem(const HttpRequest& req, HttpReply& res) const;
    void putLibraryItem(const HttpRequest& req, HttpReply& res) const;
    void getWeatherPoint(const HttpRequest& req, HttpReply& res) const;
    void getWeatherHorizon(const HttpRequest& req, HttpReply& res) const;
    void getMapConfig(const HttpRequest& req, HttpReply& res) const;
    void getModules(const HttpRequest& req, HttpReply& res) const;
    void getModuleFile(const HttpRequest& req, HttpReply& res) const;

private:
    /// The library kind of a path, checked: HttpError 404 for an unknown or disabled kind.
    [[nodiscard]] std::string libraryKind(const std::string& kind) const;
    /// Whether the request may read an alpha module's file: the admin, or a valid grant.
    [[nodiscard]] bool mayReadAlpha(const HttpRequest& req, const std::string& moduleId) const;

    const Services& s_;
    const AdminToken& admin_;
};

}  // namespace rfmap
