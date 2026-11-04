#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace httplib {
class Client;
class Result;
}  // namespace httplib

namespace rfmap::cli {

class Sha256;

using json = nlohmann::json;

/// The server answered with an error status (after any retries): exit status 2.
class ApiError : public std::runtime_error {
public:
    ApiError(int status, json body, const std::string& what)
        : std::runtime_error(what), status_(status), body_(std::move(body)) {}
    [[nodiscard]] int status() const { return status_; }
    /// The server's error document ({message, issues?, ...}), or the raw text as a string.
    [[nodiscard]] const json& body() const { return body_; }

private:
    int status_;
    json body_;
};

/// The server could not be reached, or did not answer in time: exit status 3.
class ConnectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A file the client writes (a download) cannot be: exit status 73.
class LocalFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// How to reach the server and how patient to be with it.
struct ClientOptions {
    /// "http(s)://host[:port][/prefix]": the API is at <prefix>/api/v1.
    std::string server;
    std::string token;  ///< Bearer token (a service token); empty = none
    std::chrono::seconds timeout{300};
    /// Retries after 429 or 503 (waiting Retry-After, at most 60 s each) and, for GET, after a
    /// connection that failed.
    int retries = 3;
    std::string userAgent = "rfmap-cli";
};

/// The HTTP side of the tool: JSON in and out of /api/v1, retries, and streamed downloads.
class ApiClient {
public:
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    /// `sleep` waits between retries; tests pass one that only records.
    ApiClient(ClientOptions options, Sleep sleep);
    ~ApiClient();
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;
    ApiClient(ApiClient&&) = delete;
    ApiClient& operator=(ApiClient&&) = delete;

    /// `path` is below /api/v1 and may carry a query ("/weather/point?lat=..").
    [[nodiscard]] json get(const std::string& path);
    [[nodiscard]] json post(const std::string& path, const json& body);
    /// The answer's status with its body: for calls whose 202 or 409 the caller reads itself.
    [[nodiscard]] std::pair<int, json> postWithStatus(const std::string& path, const json& body);
    [[nodiscard]] json postBytes(const std::string& path, const std::string& bytes, const std::string& contentType);
    [[nodiscard]] json put(const std::string& path, const json& body);
    /// DELETE; a 204 answers null.
    json remove(const std::string& path);
    struct Download {
        std::uintmax_t bytes = 0;
        std::string sha256;  ///< of the bytes written, lowercase hex
    };
    /// Writes the answer's body to `file` as it arrives (packages can be gigabytes), hashing it on
    /// the way. The file only appears when the download is complete.
    Download download(const std::string& path, const std::filesystem::path& file);

    /// "scheme://host:port" and the path prefix of a server URL; ConnectionError when it is not one.
    static std::pair<std::string, std::string> splitServer(std::string_view server);
    /// The seconds the server asks to wait: retryAfterS in the body, else the Retry-After header.
    [[nodiscard]] static std::chrono::seconds retryAfter(const json& body, std::string_view header);

private:
    struct Answer {
        int status = 0;
        std::string body;
        std::string retryAfterHeader;
        std::string location;  ///< of a redirect
    };
    /// The answer of a request; ConnectionError when there was none.
    [[nodiscard]] static Answer answerOf(const httplib::Result& r);
    /// Runs `send(client)` with the retry policy and turns an error status into ApiError.
    template <typename Send>
    Answer exchange(const std::string& method, const std::string& path, const Send& send, bool idempotent);
    /// The ApiError for an answer of 3xx or worse.
    [[noreturn]] static void fail(const std::string& method, const std::string& path, const Answer& a);
    /// One attempt of a download: the body into `partial`, hashed into `hash`, counted in `written`.
    static Answer fetchTo(httplib::Client& c, const std::string& target, const std::filesystem::path& partial,
                          Sha256& hash, std::uintmax_t& written);
    [[nodiscard]] static json parse(const std::string& body);
    [[nodiscard]] std::string url(const std::string& path) const { return prefix_ + "/api/v1" + path; }

    ClientOptions options_;
    Sleep sleep_;
    std::string prefix_;
    std::unique_ptr<httplib::Client> http_;
};

}  // namespace rfmap::cli
