#include "api_client.hpp"

#include "digest.hpp"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>

namespace rfmap::cli {
namespace {

/// The longest wait between two attempts, whatever the server asks for.
constexpr std::chrono::seconds kMaxRetryWait{60};

}  // namespace

ApiClient::ApiClient(ClientOptions options, Sleep sleep) : options_(std::move(options)), sleep_(std::move(sleep)) {
    auto [origin, prefix] = splitServer(options_.server);
    prefix_ = std::move(prefix);
    http_ = std::make_unique<httplib::Client>(origin);
    http_->set_connection_timeout(std::chrono::seconds(10));
    http_->set_read_timeout(options_.timeout);
    http_->set_write_timeout(options_.timeout);
    httplib::Headers headers{{"Accept", "application/json"}, {"User-Agent", options_.userAgent}};
    if (!options_.token.empty()) headers.emplace("Authorization", "Bearer " + options_.token);
    http_->set_default_headers(std::move(headers));
}

ApiClient::~ApiClient() = default;

std::pair<std::string, std::string> ApiClient::splitServer(std::string_view server) {
    // Plain http is for a server on this host or its network, as in the image's own
    // `rfmap --server http://127.0.0.1:8080`; https is checked against the system's CAs.
    if (!server.starts_with("http://") && !server.starts_with("https://")) {  // NOSONAR(cpp:S5332): a scheme check, not a connection
        throw ConnectionError(std::format("the server must be an http:// or https:// URL, not \"{}\"", server));
    }
    const auto hostStart = server.find("://") + 3;
    const auto pathStart = server.find('/', hostStart);
    std::string origin(server.substr(0, pathStart));
    std::string prefix(pathStart == std::string_view::npos ? std::string_view{} : server.substr(pathStart));
    while (prefix.ends_with('/')) prefix.pop_back();
    if (origin.size() == hostStart) throw ConnectionError(std::format("the server URL has no host: {}", server));
    return {origin, prefix};
}

std::chrono::seconds ApiClient::retryAfter(const json& body, std::string_view header) {
    long long seconds = 1;
    if (body.is_object() && body.contains("retryAfterS") && body.at("retryAfterS").is_number()) {
        seconds = static_cast<long long>(std::ceil(body.at("retryAfterS").get<double>()));
    } else if (!header.empty()) {
        long long parsed = 0;
        const auto [ptr, ec] = std::from_chars(header.data(), header.data() + header.size(), parsed);
        if (ec == std::errc{} && ptr == header.data() + header.size()) seconds = parsed;
    }
    return std::clamp(std::chrono::seconds(seconds), std::chrono::seconds(1), kMaxRetryWait);
}

json ApiClient::parse(const std::string& body) {
    if (body.empty()) return nullptr;
    try {
        return json::parse(body);
    } catch (const json::exception&) {
        return body;
    }
}

ApiClient::Answer ApiClient::answerOf(const httplib::Result& r) {
    if (!r) throw ConnectionError(httplib::to_string(r.error()));
    return {r->status, r->body, r->get_header_value("Retry-After"), r->get_header_value("Location")};
}

void ApiClient::fail(const std::string& method, const std::string& path, const Answer& a) {
    const std::string what = std::format("{} {}: {} {}", method, path, a.status, httplib::status_message(a.status));
    if (a.status < 400) {
        // Followed, a redirect would carry the token to wherever it points: name it instead.
        throw ApiError(a.status, nullptr,
                       std::format("{}: the server redirects to {}; give that address as --server", what,
                                   a.location.empty() ? "another URL" : a.location));
    }
    const json body = parse(a.body);
    std::string message;
    if (body.is_object()) {
        message = body.value("message", "");
    } else if (body.is_string()) {
        message = body.get<std::string>();
    }
    throw ApiError(a.status, body, message.empty() ? what : std::format("{}: {}", what, message));
}

template <typename Send>
ApiClient::Answer ApiClient::exchange(const std::string& method, const std::string& path, const Send& send,
                                      bool idempotent) const {
    for (int attempt = 0;; ++attempt) {
        const bool mayRetry = attempt < options_.retries;
        Answer a;
        try {
            a = send(*http_);
        } catch (const ConnectionError&) {
            if (!idempotent || !mayRetry) throw;
            sleep_(std::chrono::seconds(1LL << std::min(attempt, 5)));
            continue;
        }
        if ((a.status == 429 || a.status == 503) && mayRetry) {
            sleep_(retryAfter(parse(a.body), a.retryAfterHeader));
            continue;
        }
        if (a.status >= 300) fail(method, path, a);
        return a;
    }
}

json ApiClient::get(const std::string& path) const {
    const std::string target = url(path);
    return parse(exchange("GET", path, [&target](httplib::Client& c) { return answerOf(c.Get(target)); }, true).body);
}

std::pair<int, json> ApiClient::postWithStatus(const std::string& path, const json& body) const {
    const std::string target = url(path);
    const std::string text = body.dump();
    const Answer a = exchange(
        "POST", path, [&target, &text](httplib::Client& c) { return answerOf(c.Post(target, text, "application/json")); },
        false);
    return {a.status, parse(a.body)};
}

json ApiClient::post(const std::string& path, const json& body) const { return postWithStatus(path, body).second; }

json ApiClient::postBytes(const std::string& path, const std::string& bytes, const std::string& contentType) const {
    const std::string target = url(path);
    const auto send = [&target, &bytes, &contentType](httplib::Client& c) {
        return answerOf(c.Post(target, bytes, contentType));
    };
    return parse(exchange("POST", path, send, false).body);
}

json ApiClient::put(const std::string& path, const json& body) const {
    const std::string target = url(path);
    const std::string text = body.dump();
    const auto send = [&target, &text](httplib::Client& c) { return answerOf(c.Put(target, text, "application/json")); };
    return parse(exchange("PUT", path, send, false).body);
}

json ApiClient::remove(const std::string& path) const {
    const std::string target = url(path);
    return parse(exchange("DELETE", path, [&target](httplib::Client& c) { return answerOf(c.Delete(target)); }, true)
                     .body);
}

ApiClient::Answer ApiClient::fetchTo(httplib::Client& c, const std::string& target, const std::filesystem::path& partial,
                                     Sha256& hash) {
    // A fresh file per attempt: a retry never appends to what a failed one left.
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) throw LocalFileError("cannot write " + partial.string());
    hash.restart();
    int status = 0;
    std::string errorBody;
    const auto onResponse = [&status](const httplib::Response& res) {
        status = res.status;
        return true;
    };
    const auto onData = [&status, &errorBody, &out, &hash](const char* data, std::size_t size) {
        if (status != 200) {
            errorBody.append(data, size);
            return true;
        }
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out) && hash.add(std::string_view(data, size));
    };
    const auto r = c.Get(target, onResponse, onData);
    out.close();
    if (status == 200 && !out) throw LocalFileError("cannot write " + partial.string());
    Answer a = answerOf(r);
    if (a.status != 200) a.body = std::move(errorBody);
    return a;
}

ApiClient::Download ApiClient::download(const std::string& path, const std::filesystem::path& file) const {
    const std::string target = url(path);
    const std::filesystem::path partial = file.string() + ".part";
    Download d;
    Sha256 hash;
    const auto send = [&target, &partial, &hash](httplib::Client& c) { return fetchTo(c, target, partial, hash); };
    try {
        exchange("GET", path, send, true);
        d.bytes = hash.size();
        d.sha256 = hash.hex();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw;
    }
    std::error_code renamed;
    std::filesystem::rename(partial, file, renamed);
    if (renamed) throw LocalFileError(std::format("cannot write {}: {}", file.string(), renamed.message()));
    return d;
}

}  // namespace rfmap::cli
