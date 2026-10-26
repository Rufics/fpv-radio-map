#pragma once

#include "calc_gate.hpp"
#include "request.hpp"
#include "services.hpp"

#include <memory>
#include <string_view>

namespace rfmap {

/// Health, missions, share links, calculations, jobs and the elevation profile.
class MissionApi {
public:
    explicit MissionApi(const Services& services);

    /// The bulkhead POST /calc/path and /calc/route run in.
    [[nodiscard]] const CalcGate& calcGate() const { return *calcGate_; }

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
    [[nodiscard]] Principal missionSession(const HttpRequest& req, std::string_view missionId) const;
    /// The mission's name for a link holder, or empty.
    [[nodiscard]] std::string missionName(const std::string& missionId) const;
    /// HttpError 422 when the grid would be larger than the server allows.
    void checkCoverageSize(const json& mission) const;
    /// HttpError 422 when the route would read more terrain samples than the server allows.
    void checkRouteSize(const json& mission) const;

    const Services& s_;
    std::unique_ptr<CalcGate> calcGate_;
};

}  // namespace rfmap
