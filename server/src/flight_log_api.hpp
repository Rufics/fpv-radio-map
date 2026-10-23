#pragma once

#include "request.hpp"
#include "services.hpp"

namespace rfmap {

/// Flight logs: decode a file to preview it, and the flights kept in a mission.
class FlightLogApi {
public:
    explicit FlightLogApi(const Services& services) : s_(services) {}

    void postDecode(const HttpRequest& req, HttpReply& res) const;
    void getList(const HttpRequest& req, HttpReply& res) const;
    void postImport(const HttpRequest& req, HttpReply& res) const;
    void getFlight(const HttpRequest& req, HttpReply& res) const;
    void deleteFlight(const HttpRequest& req, HttpReply& res) const;

private:
    /// The session behind the bearer token, checked against the mission in the path (HttpError
    /// 403 for another mission) and, when `edit`, for the edit role (403 for a read-only link).
    [[nodiscard]] Principal principal(const HttpRequest& req, bool edit) const;
    /// The uploaded file's name from the query, or a placeholder.
    [[nodiscard]] static std::string fileName(const HttpRequest& req);

    const Services& s_;
};

}  // namespace rfmap
