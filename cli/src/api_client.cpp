#include "api_client.hpp"

#include <httplib.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>

namespace rfmap::cli {
namespace {

/// The longest wait between two attempts, whatever the server asks for.
constexpr std::chrono::seconds kMaxRetryWait{60};

std::string describe(httplib::Error e) { return httplib::to_string(e); }

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
    const auto scheme = server.find("://");
    if (scheme == std::string_view::npos || (server.substr(0, scheme) != "http" && server.substr(0, scheme) != "https")) {
        throw ConnectionError("the server must be an http:// or https:// URL, not \"" + std::string(server) + "\"");
    }
    const auto pathStart = server.find('/', scheme + 3);
    std::string origin(server.substr(0, pathStart));
    std::string prefix(pathStart == std::string_view::npos ? std::string_view{} : server.substr(pathStart));
    while (prefix.ends_with('/')) prefix.pop_back();
    if (origin.size() == scheme + 3) throw ConnectionError("the server URL has no host: " + std::string(server));
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

ApiClient::Answer ApiClient::answerOf(int status, std::string body, const httplib::Response& r) {
    return {status, std::move(body), r.get_header_value("Retry-After"), r.get_header_value("Location")};
}

ApiClient::Answer ApiClient::exchange(const std::string& method, const std::string& path,
                                      const std::function<Answer(httplib::Client&)>& send, bool idempotent) {
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
        if (a.status >= 300 && a.status < 400) {
            // Followed, a redirect would carry the token to wherever it points: name it instead.
            throw ApiError(a.status, nullptr,
                           method + " " + path + ": " + std::to_string(a.status) + " " + httplib::status_message(a.status) +
                               ": the server redirects to " + (a.location.empty() ? "another URL" : a.location) +
                               "; give that address as --server");
        }
        if (a.status >= 400) {
            const json body = parse(a.body);
            std::string message = body.is_object() ? body.value("message", "") : body.is_string() ? body.get<std::string>() : "";
            std::string what = method + " " + path + ": " + std::to_string(a.status) + " " + httplib::status_message(a.status);
            if (!message.empty()) what += ": " + message;
            throw ApiError(a.status, body, what);
        }
        return a;
    }
}

json ApiClient::get(const std::string& path) {
    const std::string target = url(path);
    return parse(exchange("GET", path,
                          [&target](httplib::Client& c) {
                              const auto r = c.Get(target);
                              if (!r) throw ConnectionError(describe(r.error()));
                              return answerOf(r->status, r->body, *r);
                          },
                          true)
                     .body);
}

std::pair<int, json> ApiClient::postWithStatus(const std::string& path, const json& body) {
    const std::string target = url(path);
    const std::string text = body.dump();
    const Answer a = exchange("POST", path,
                              [&target, &text](httplib::Client& c) {
                                  const auto r = c.Post(target, text, "application/json");
                                  if (!r) throw ConnectionError(describe(r.error()));
                                  return answerOf(r->status, r->body, *r);
                              },
                              false);
    return {a.status, parse(a.body)};
}

json ApiClient::post(const std::string& path, const json& body) { return postWithStatus(path, body).second; }

json ApiClient::postBytes(const std::string& path, const std::string& bytes, const std::string& contentType) {
    const std::string target = url(path);
    return parse(exchange("POST", path,
                          [&](httplib::Client& c) {
                              const auto r = c.Post(target, bytes, contentType);
                              if (!r) throw ConnectionError(describe(r.error()));
                              return answerOf(r->status, r->body, *r);
                          },
                          false)
                     .body);
}

json ApiClient::put(const std::string& path, const json& body) {
    const std::string target = url(path);
    const std::string text = body.dump();
    return parse(exchange("PUT", path,
                          [&target, &text](httplib::Client& c) {
                              const auto r = c.Put(target, text, "application/json");
                              if (!r) throw ConnectionError(describe(r.error()));
                              return answerOf(r->status, r->body, *r);
                          },
                          false)
                     .body);
}

json ApiClient::remove(const std::string& path) {
    const std::string target = url(path);
    return parse(exchange("DELETE", path,
                          [&target](httplib::Client& c) {
                              const auto r = c.Delete(target);
                              if (!r) throw ConnectionError(describe(r.error()));
                              return answerOf(r->status, r->body, *r);
                          },
                          true)
                     .body);
}

ApiClient::Download ApiClient::download(const std::string& path, const std::filesystem::path& file) {
    const std::string target = url(path);
    const std::filesystem::path partial = file.string() + ".part";
    std::uintmax_t written = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    const auto send = [&](httplib::Client& c) {
        // A fresh file per attempt: a retry never appends to what a failed one left.
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) throw LocalFileError("cannot write " + partial.string());
        written = 0;
        if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA-256 failed");
        int status = 0;
        std::string errorBody;
        const auto r = c.Get(
            target,
            [&status](const httplib::Response& res) {
                status = res.status;
                return true;
            },
            [&](const char* data, std::size_t size) {
                if (status != 200) {
                    errorBody.append(data, size);
                    return true;
                }
                out.write(data, static_cast<std::streamsize>(size));
                written += size;
                return static_cast<bool>(out) && EVP_DigestUpdate(hash.get(), data, size) == 1;
            });
        if (!r) throw ConnectionError(describe(r.error()));
        out.close();
        if (r->status == 200 && !out) throw LocalFileError("cannot write " + partial.string());
        return answerOf(r->status, r->status == 200 ? std::string() : errorBody, *r);
    };
    try {
        exchange("GET", path, send, true);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &size) != 1) throw std::runtime_error("SHA-256 failed");
    std::error_code renamed;
    std::filesystem::rename(partial, file, renamed);
    if (renamed) throw LocalFileError("cannot write " + file.string() + ": " + renamed.message());
    static constexpr std::string_view kHex = "0123456789abcdef";
    Download d{written, {}};
    for (unsigned int i = 0; i < size; ++i) {
        d.sha256 += kHex[digest[i] >> 4U];
        d.sha256 += kHex[digest[i] & 0x0fU];
    }
    return d;
}

}  // namespace rfmap::cli
