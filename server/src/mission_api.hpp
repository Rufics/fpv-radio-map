#pragma once

#include "request.hpp"
#include "services.hpp"

namespace rfmap {

/// Health, missions, share links, calculations, jobs and the elevation profile.
class MissionApi {
public:
    explicit MissionApi(const Services& services) : s_(services) {}

    void getHealth(const HttpRequest& req, HttpReply& res) const;
    void postMission(const HttpRequest& req, HttpReply& res) const;
    void getMission(const HttpRequest& req, HttpReply& res) const;
    void putMission(const HttpRequest& req, HttpReply& res) const;
    void getLinks(const HttpRequest& req, HttpReply& res) const;
    void postLink(const HttpRequest& req, HttpReply& res) const;
    void deleteLink(const HttpRequest& req, HttpReply& res) const;
    void getLink(const HttpRequest& req, HttpReply& res) const;
    void postLinkOpen(const HttpRequest& req, HttpReply& res) const;
    void postCalcPath(const HttpRequest& req, HttpReply& res) const;
    void postCalcRoute(const HttpRequest& req, HttpReply& res) const;
    void postCalcCoverage(const HttpRequest& req, HttpReply& res) const;
    void getJob(const HttpRequest& req, HttpReply& res) const;
    void deleteJob(const HttpRequest& req, HttpReply& res) const;
    void getJobGrid(const HttpRequest& req, HttpReply& res) const;
    void getElevationProfile(const HttpRequest& req, HttpReply& res) const;

private:
    /// The session behind the bearer token; HttpError 401 without one.
    [[nodiscard]] Principal session(const HttpRequest& req) const;
    /// session(), and HttpError 403 unless it belongs to the mission in the path.
    [[nodiscard]] Principal missionSession(const HttpRequest& req, const std::string& missionId) const;
    /// The mission's name for a link holder, or empty.
    [[nodiscard]] std::string missionName(const std::string& missionId) const;
    /// HttpError 422 when the grid would be larger than the server allows.
    void checkCoverageSize(const json& mission) const;

    const Services& s_;
};

}  // namespace rfmap
