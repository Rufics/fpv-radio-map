#include "request.hpp"

#include "security.hpp"

#include <httplib.h>
#include <rfmod/bounded_json.hpp>

#include <charconv>
#include <cmath>
#include <regex>

namespace rfmap {

HttpRequest::HttpRequest(const httplib::Request& request, const Config::Server& server)
    : request_(request), server_(server) {}

const std::string& HttpRequest::method() const { return request_.method; }
const std::string& HttpRequest::path() const { return request_.path; }
std::string HttpRequest::match(std::size_t index) const { return request_.matches[index]; }

std::string HttpRequest::header(const char* name) const {
    return request_.has_header(name) ? request_.get_header_value(name) : "";
}

std::string HttpRequest::bearer() const {
    if (const std::string h = header("Authorization"); h.starts_with("Bearer ")) return h.substr(7);
    return {};
}

bool HttpRequest::hasParam(const char* name) const { return request_.has_param(name); }
std::string HttpRequest::param(const char* name) const { return request_.get_param_value(name); }

double HttpRequest::number(const char* name) const {
    if (!request_.has_param(name)) throw HttpError(400, std::string("query parameter ") + name + " is required");
    try {
        std::size_t used = 0;
        const std::string v = request_.get_param_value(name);
        const double d = std::stod(v, &used);
        if (used != v.size() || !std::isfinite(d)) throw std::invalid_argument("x");
        return d;
    } catch (const std::invalid_argument&) {
        throw HttpError(400, std::string("query parameter ") + name + " must be a number");
    } catch (const std::out_of_range&) {
        throw HttpError(400, std::string("query parameter ") + name + " is out of range");
    }
}

rfcore::GeoPoint HttpRequest::latLon(const char* name) const {
    if (!request_.has_param(name)) throw HttpError(400, std::string("query parameter ") + name + " is required (lat,lon)");
    return parseLatLon(request_.get_param_value(name));
}

rfcore::GeoPoint HttpRequest::parseLatLon(const std::string& text) {
    static const std::regex re(
        R"(^\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*,\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*$)");
    std::smatch m;
    if (text.size() > 64 || !std::regex_match(text, m, re)) throw HttpError(400, "expected lat,lon, got '" + text + "'");
    const double lat = std::stod(m[1]);
    const double lon = std::stod(m[2]);
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) throw HttpError(400, "lat,lon out of range");
    return {lat, lon};
}

std::optional<TimePoint> HttpRequest::time(const char* name) const {
    if (!request_.has_param(name)) return std::nullopt;
    const auto parsed = UtcTime::parse(request_.get_param_value(name));
    if (!parsed) throw HttpError(400, std::string(name) + " is not an ISO 8601 date-time");
    return parsed;
}

int HttpRequest::ifMatchRevision() const {
    std::string match = header("If-Match");
    std::erase_if(match, [](char c) { return c == '"' || c == 'W' || c == '/'; });
    try {
        return std::stoi(match);
    } catch (const std::invalid_argument&) {
        throw HttpError(428, "If-Match with the revision you edited is required");
    } catch (const std::out_of_range&) {
        throw HttpError(428, "If-Match with the revision you edited is required");
    }
}

json HttpRequest::body() const {
    if (request_.body.size() > server_.maxBodyBytes) throw HttpError(413, "request body too large");
    try {
        return rfmod::BoundedJson::parse(request_.body);
    } catch (const json::exception& e) {
        throw HttpError(400, std::string("body is not valid JSON: ") + e.what());
    } catch (const rfmod::JsonTooDeepError& e) {
        throw HttpError(400, std::string("body is not accepted: ") + e.what());
    }
}

const std::string& HttpRequest::file() const {
    if (request_.body.size() > server_.maxUploadBytes) throw HttpError(413, "the file is larger than this server takes");
    return request_.body;
}

std::string HttpRequest::client() const {
    if (server_.trustProxy && request_.has_header("X-Forwarded-For")) {
        // The proxy appends the address it saw; earlier entries are the client's own claims.
        const std::string xff = request_.get_header_value("X-Forwarded-For");
        const auto comma = xff.rfind(',');
        std::string last = comma == std::string::npos ? xff : xff.substr(comma + 1);
        last.erase(0, last.find_first_not_of(' '));
        if (!last.empty()) return last;
    }
    return request_.remote_addr;
}

std::string HttpRequest::redacted(const std::string& path) {
    static const std::regex link("/links/[^/]+(/open)?$");
    return std::regex_replace(path, link, "/links/***$1");
}

void HttpReply::send(int status, const json& body) {
    response_.status = status;
    response_.set_content(body.dump(), "application/json");
}

void HttpReply::error(const HttpError& e) {
    json b = e.body();
    b["message"] = e.what();
    if ((e.status() == 429 || e.status() == 503) && b.contains("retryAfterS")) {
        response_.set_header("Retry-After", std::to_string(b["retryAfterS"].get<long long>()));
    }
    send(e.status(), b);
}

void HttpReply::noContent() { response_.status = 204; }

void HttpReply::text(int status, const std::string& body, const char* contentType) {
    response_.status = status;
    response_.set_content(body, contentType);
}

void HttpReply::header(const char* name, const std::string& value) { response_.set_header(name, value); }

int HttpReply::status() const { return response_.status; }

std::size_t BodyLimit::forPath(std::string_view path) const {
    const bool upload = path.ends_with("/flightlogs") || path.ends_with("/flightlogs/decode");
    return upload ? server_.maxUploadBytes : server_.maxBodyBytes;
}

bool BodyLimit::exceeded(std::string_view path, std::string_view contentLength) const {
    std::size_t declared = 0;
    const char* end = contentLength.data() + contentLength.size();
    const auto [ptr, ec] = std::from_chars(contentLength.data(), end, declared);
    if (ec == std::errc::result_out_of_range) return true;
    return ec == std::errc{} && ptr == end && declared > forPath(path);
}

bool AdminToken::presentedBy(const HttpRequest& request) const {
    const std::string t = request.bearer();
    return configured() && !t.empty() && Secrets::constantTimeEqual(t, token_);
}

void AdminToken::require(const HttpRequest& request) const {
    if (!configured()) throw HttpError(403, "the admin API is disabled on this server");
    if (!presentedBy(request)) throw HttpError(401, "admin token required");
}

}  // namespace rfmap
