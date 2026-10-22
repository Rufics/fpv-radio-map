#pragma once

#include "common.hpp"
#include "config.hpp"

#include <rfcore/geodesy.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace httplib {
struct Request;
struct Response;
}  // namespace httplib

namespace rfmap {

/// A request as the handlers read it: every reading validates, and names what was wrong in
/// the HttpError it throws, so no handler parses on its own.
class HttpRequest {
public:
    HttpRequest(const httplib::Request& request, const Config::Server& server);

    [[nodiscard]] const std::string& method() const;
    [[nodiscard]] const std::string& path() const;
    /// Capture `index` of the route pattern (1 is the first).
    [[nodiscard]] std::string match(std::size_t index) const;
    [[nodiscard]] std::string header(const char* name) const;
    /// The token of an "Authorization: Bearer ..." header, else empty.
    [[nodiscard]] std::string bearer() const;
    [[nodiscard]] bool hasParam(const char* name) const;
    [[nodiscard]] std::string param(const char* name) const;
    /// A required finite number in the query (400 otherwise).
    [[nodiscard]] double number(const char* name) const;
    /// A required "lat,lon" pair in the query, within the world (400 otherwise).
    [[nodiscard]] rfcore::GeoPoint latLon(const char* name) const;
    /// An optional ISO 8601 date-time in the query (400 when malformed).
    [[nodiscard]] std::optional<TimePoint> time(const char* name) const;
    /// The revision in an If-Match header ("3", W/"3"); HttpError 428 without one.
    [[nodiscard]] int ifMatchRevision() const;
    /// The body as JSON: 413 above the configured size, 400 when not JSON.
    [[nodiscard]] json body() const;
    /// The client's address: the last X-Forwarded-For hop behind a trusted proxy, else the peer.
    [[nodiscard]] std::string client() const;
    /// The path with link tokens replaced, for the log.
    [[nodiscard]] static std::string redacted(const std::string& path);

    /// A "lat,lon" text as a point within the world; HttpError 400 otherwise.
    [[nodiscard]] static rfcore::GeoPoint parseLatLon(const std::string& text);

private:
    const httplib::Request& request_;
    const Config::Server& server_;
};

/// The reply the handlers write: JSON bodies, uniform errors, files with their headers.
class HttpReply {
public:
    explicit HttpReply(httplib::Response& response) : response_(response) {}

    void send(int status, const json& body);
    /// {message, ...body} with the error's status and Retry-After on 429 and 503.
    void error(const HttpError& e);
    void noContent();
    void text(int status, const std::string& body, const char* contentType);
    void header(const char* name, const std::string& value);
    [[nodiscard]] int status() const;

private:
    httplib::Response& response_;
};

/// The admin API's one credential: a bearer token from the config, compared in constant time.
class AdminToken {
public:
    explicit AdminToken(std::string token) : token_(std::move(token)) {}

    [[nodiscard]] bool configured() const { return !token_.empty(); }
    [[nodiscard]] bool presentedBy(const HttpRequest& request) const;
    /// HttpError 403 when the admin API is off, 401 when the token is missing or wrong.
    void require(const HttpRequest& request) const;

private:
    std::string token_;
};

}  // namespace rfmap
