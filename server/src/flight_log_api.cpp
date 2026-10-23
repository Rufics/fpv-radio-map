#include "flight_log_api.hpp"

namespace rfmap {

Principal FlightLogApi::principal(const HttpRequest& req, bool edit) const {
    const Principal p = s_.auth().authenticate(req.bearer());
    if (p.missionId != req.match(1)) throw HttpError(403, "this session belongs to another mission");
    if (edit && p.role != Role::Edit) throw HttpError(403, "this is a read-only link");
    return p;
}

std::string FlightLogApi::fileName(const HttpRequest& req) {
    return req.hasParam("fileName") ? req.param("fileName") : "flight.log";
}

void FlightLogApi::postDecode(const HttpRequest& req, HttpReply& res) const {
    (void)s_.auth().authenticate(req.bearer());
    res.send(200, s_.flightLogs().preview(req.file(), fileName(req)));
}

void FlightLogApi::getList(const HttpRequest& req, HttpReply& res) const {
    const Principal p = principal(req, false);
    res.send(200, s_.flightLogs().list(p.missionId));
}

void FlightLogApi::postImport(const HttpRequest& req, HttpReply& res) const {
    const Principal p = principal(req, true);
    res.send(201, s_.flightLogs().importFile(p.missionId, req.file(), fileName(req)));
}

void FlightLogApi::getFlight(const HttpRequest& req, HttpReply& res) const {
    const Principal p = principal(req, false);
    const auto m = s_.missions().get(p.missionId);
    if (!m) throw HttpError(404, "no such mission");
    res.send(200, s_.flightLogs().view(*m, req.match(2)));
}

void FlightLogApi::deleteFlight(const HttpRequest& req, HttpReply& res) const {
    const Principal p = principal(req, true);
    s_.flightLogs().remove(p.missionId, req.match(2));
    res.noContent();
}

}  // namespace rfmap
