#include "mission_api.hpp"

#include <format>

namespace rfmap {

Principal MissionApi::session(const HttpRequest& req) const { return s_.auth().authenticate(req.bearer()); }

Principal MissionApi::missionSession(const HttpRequest& req, std::string_view missionId) const {
    const Principal p = session(req);
    if (p.missionId != missionId) throw HttpError(403, "this session belongs to another mission");
    return p;
}

std::string MissionApi::missionName(const std::string& missionId) const {
    const auto m = s_.missions().get(missionId);
    return m ? m->doc.value("name", "") : "";
}

void MissionApi::getHealth(const HttpRequest&, HttpReply& res) const {
    res.send(200, {{"ok", true},
                   {"version", RFMAP_HOST_VERSION},
                   {"dem", s_.dem() != nullptr},
                   {"landcover", s_.landcover() != nullptr},
                   {"weather", s_.weather().configured()}});
}

// ---- missions ----

void MissionApi::postMission(const HttpRequest& req, HttpReply& res) const {
    const json doc = req.body();
    s_.schemas().require("mission", doc);
    const AuthService::OwnerKey key = AuthService::newOwnerKey();
    const std::string id = s_.missions().create(doc, key.hash);
    const std::string sess = s_.auth().createSession(id, Role::Edit, std::nullopt);
    res.header("ETag", "\"1\"");
    res.send(201, {{"missionId", id}, {"ownerKey", key.plain}, {"session", sess}, {"revision", 1}});
}

void MissionApi::getMission(const HttpRequest& req, HttpReply& res) const {
    const std::string id = req.match(1);
    const Principal p = missionSession(req, id);
    const auto m = s_.missions().get(id);
    if (!m) throw HttpError(404, "no such mission");
    json out = {{"mission", m->doc}, {"role", toString(p.role)}, {"revision", m->revision}};
    if (m->lastResult.has_value()) out["lastResult"] = *m->lastResult;
    res.header("ETag", std::format("\"{}\"", m->revision));
    res.send(200, out);
}

void MissionApi::putMission(const HttpRequest& req, HttpReply& res) const {
    const std::string id = req.match(1);
    if (const Principal p = missionSession(req, id); p.role != Role::Edit) throw HttpError(403, "this is a read-only link");
    const int expected = req.ifMatchRevision();
    const json doc = req.body();
    s_.schemas().require("mission", doc);
    const auto rev = s_.missions().save(id, doc, expected);
    if (!rev.has_value()) {
        const auto cur = s_.missions().get(id);
        if (!cur) throw HttpError(404, "no such mission");
        throw HttpError(409, "someone saved this mission in between", {{"revision", cur->revision}});
    }
    res.header("ETag", std::format("\"{}\"", *rev));
    res.send(200, {{"revision", *rev}});
}

// ---- links (owner key) ----

void MissionApi::getLinks(const HttpRequest& req, HttpReply& res) const {
    json out = json::array();
    for (const auto& l : s_.auth().listLinks(req.match(1), req.header("X-Owner-Key"))) {
        out.push_back(AuthService::linkJson(l));
    }
    res.send(200, out);
}

void MissionApi::postLink(const HttpRequest& req, HttpReply& res) const {
    const std::string id = req.match(1);
    const std::string key = req.header("X-Owner-Key");
    s_.auth().requireOwner(id, key);  // before looking at the body, so strangers learn nothing
    const json b = req.body();
    s_.schemas().require("link-create", b);
    std::optional<TimePoint> expires;
    if (b.contains("expiresAt")) {
        expires = UtcTime::parse(b.at("expiresAt").get<std::string>());
        if (!expires.has_value()) throw HttpError(422, "expiresAt is not ISO 8601 UTC");
    }
    const auto created = s_.auth().createLink(id, key, roleFromString(b.at("role").get<std::string>()),
                                              b.at("pin").get<std::string>(), b.value("label", ""), expires);
    json out = AuthService::linkJson(created.record);
    out["token"] = created.token;
    res.send(201, out);
}

void MissionApi::deleteLink(const HttpRequest& req, HttpReply& res) const {
    s_.auth().revokeLink(req.match(1), req.header("X-Owner-Key"), req.match(2));
    res.noContent();
}

void MissionApi::getLink(const HttpRequest& req, HttpReply& res) const {
    const LinkRecord l = s_.auth().peekLink(req.match(1));
    json out = {{"role", toString(l.role)}, {"missionName", missionName(l.missionId)}};
    if (l.expiresAt.has_value()) out["expiresAt"] = UtcTime::format(*l.expiresAt);
    if (l.lockedUntil.has_value() && *l.lockedUntil > s_.clock().now()) {
        out["pausedUntil"] = UtcTime::format(*l.lockedUntil);
    }
    res.send(200, out);
}

void MissionApi::postLinkOpen(const HttpRequest& req, HttpReply& res) const {
    const json b = req.body();
    if (!b.contains("pin") || !b.at("pin").is_string()) throw HttpError(400, "pin is required");
    const auto opened = s_.auth().openLink(req.match(1), b.at("pin"));
    res.send(200, {{"session", opened.session},
                   {"missionId", opened.missionId},
                   {"role", toString(opened.role)},
                   {"missionName", missionName(opened.missionId)}});
}

// ---- calc ----

void MissionApi::postCalcPath(const HttpRequest& req, HttpReply& res) const {
    (void)session(req);
    const json b = req.body();
    s_.schemas().require("calc-path", b);
    res.send(200, s_.calc().path(b));
}

void MissionApi::postCalcRoute(const HttpRequest& req, HttpReply& res) const {
    const Principal p = session(req);
    const json mission = req.body();
    s_.schemas().require("mission", mission);
    std::optional<int> revision;
    if (const auto stored = s_.missions().get(p.missionId); stored.has_value() && stored->doc == mission) {
        revision = stored->revision;
    }
    const json result = s_.calc().route(mission, revision);
    if (revision.has_value() && p.role == Role::Edit) s_.missions().storeResultIfCurrent(p.missionId, mission, result);
    res.send(200, result);
}

void MissionApi::checkCoverageSize(const json& mission) const {
    const std::size_t cells = Calculator::coverageCells(mission);
    if (cells == 0) throw HttpError(422, "coverage needs coverage.radiusM, coverage.cellM and coverage.droneAglM");
    const std::size_t maxCells = s_.config().coverage.maxCells;
    if (cells > maxCells) {
        throw HttpError(422, std::format("the grid would have {} cells; this server allows {}. Use a larger cellM or a "
                                         "smaller radius.",
                                         cells, maxCells));
    }
}

void MissionApi::postCalcCoverage(const HttpRequest& req, HttpReply& res) const {
    const Principal p = session(req);
    const json mission = req.body();
    s_.schemas().require("mission", mission);
    checkCoverageSize(mission);
    if (s_.jobs().running() >= static_cast<std::size_t>(s_.config().coverage.maxJobs) * 4) {
        throw HttpError(503, "too many coverage jobs queued; try again shortly", {{"retryAfterS", 30}});
    }
    res.send(202, s_.jobs().start(p.missionId, mission));
}

void MissionApi::getJob(const HttpRequest& req, HttpReply& res) const {
    res.send(200, s_.jobs().status(session(req).missionId, req.match(1)));
}

void MissionApi::deleteJob(const HttpRequest& req, HttpReply& res) const {
    s_.jobs().cancel(session(req).missionId, req.match(1));
    res.noContent();
}

void MissionApi::getJobGrid(const HttpRequest& req, HttpReply& res) const {
    res.send(200, s_.jobs().grid(session(req).missionId, req.match(1), req.match(2)));
}

void MissionApi::getElevationProfile(const HttpRequest& req, HttpReply& res) const {
    const auto from = req.latLon("from");
    const auto to = req.latLon("to");
    const DemSource& dem = s_.links().requireDem();
    const auto built = rfcore::ProfileBuilder(dem).build(from, to);
    if (!built.profile.has_value()) {
        throw HttpError(422, "missing elevation data on part of the path; nothing is guessed",
                        s_.links().missingData(built.missing, from, to));
    }
    const auto& smp = built.profile->samples();
    res.send(200, LinkService::profileJson(*built.profile, (smp[1].distance - smp[0].distance).value()));
}

}  // namespace rfmap
