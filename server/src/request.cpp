#include "request.hpp"

#include "security.hpp"

#include <httplib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <rfmod/bounded_json.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cctype>
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

bool HttpRequest::holdsCurrent(std::string_view etag) const {
    const std::string list = header("If-None-Match");
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        std::string_view tag = std::string_view(list).substr(start, comma - start);
        tag.remove_prefix(std::min(tag.find_first_not_of(' '), tag.size()));
        tag.remove_suffix(tag.size() - std::min(tag.find_last_not_of(' ') + 1, tag.size()));
        if (tag.starts_with("W/")) tag.remove_prefix(2);
        if (tag == "*" || tag == etag) return true;
        start = comma + 1;
    }
    return false;
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

void HttpReply::notModified(const std::string& etag) {
    response_.status = 304;
    response_.set_header("ETag", etag);
}

namespace {

/// A file streamed as a response body in chunks, from any offset: httplib cuts Range requests
/// out of it, so a download resumes where it broke off. (httplib's own set_file_content does
/// open the file before the handler returns, so a missing one cannot become a JSON 404.)
class FileBody {
public:
    explicit FileBody(const std::filesystem::path& file) : fd_(::open(file.c_str(), O_RDONLY | O_CLOEXEC)) {
        struct stat st {};
        if (fd_ < 0 || ::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (fd_ >= 0) ::close(fd_);
            throw HttpError(404, "no such file");
        }
        size_ = static_cast<std::size_t>(st.st_size);
    }
    FileBody(const FileBody&) = delete;
    FileBody& operator=(const FileBody&) = delete;
    ~FileBody() { ::close(fd_); }

    [[nodiscard]] std::size_t size() const { return size_; }

    /// Writes `length` bytes from `offset` to `sink`; false when the file cannot be read.
    bool write(std::size_t offset, std::size_t length, httplib::DataSink& sink) const {
        std::array<char, kChunk> buffer{};
        const std::size_t want = std::min(length, buffer.size());
        const ssize_t got = ::pread(fd_, buffer.data(), want, static_cast<off_t>(offset));
        if (got <= 0) return false;
        return sink.write(buffer.data(), static_cast<std::size_t>(got));
    }

private:
    static constexpr std::size_t kChunk = std::size_t{64} * 1024;
    int fd_;
    std::size_t size_ = 0;
};

}  // namespace

void HttpReply::file(const std::filesystem::path& file, const char* contentType, bool ranged,
                     std::shared_ptr<const void> hold) {
    auto body = std::make_shared<const FileBody>(file);
    // httplib cuts the range only out of a 206 answer (and turns a bad range into 416).
    response_.status = ranged ? 206 : 200;
    response_.set_content_provider(body->size(), contentType,
                                   [body, hold = std::move(hold)](std::size_t offset, std::size_t length,
                                                                  httplib::DataSink& sink) {
                                       return body->write(offset, length, sink);
                                   });
}

int HttpReply::status() const { return response_.status; }

bool BodyLimit::takesFiles(std::string_view path) {
    return path.ends_with("/flightlogs") || path.ends_with("/flightlogs/decode");
}

std::size_t BodyLimit::forPath(std::string_view path) const {
    return takesFiles(path) ? server_.maxUploadBytes : server_.maxBodyBytes;
}

bool BodyLimit::exceeded(std::string_view path, std::string_view contentLength) const {
    std::size_t declared = 0;
    const std::string text(contentLength);
    const char* end = text.c_str() + text.size();
    const auto [ptr, ec] = std::from_chars(text.c_str(), end, declared);
    if (ec == std::errc::result_out_of_range) return true;
    return ec == std::errc{} && ptr == end && declared > forPath(path);
}

bool BodyLimit::encodingRefused(std::string_view contentEncoding) {
    std::string lower(contentEncoding);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !lower.empty() && lower != "identity";
}

bool BodyLimit::lengthRequired(std::string_view path, std::string_view transferEncoding) const {
    return !transferEncoding.empty() && !takesFiles(path);
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
