#pragma once

// Small pieces every part of the server uses: the HTTP error type, ISO 8601 times and the
// JSON alias. Kept here so no module needs to include the HTTP layer.

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace rfmap {

using json = nlohmann::json;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

/// An error that maps to one HTTP status. `body` is merged into the JSON response next to "message".
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& message, json body = json::object())
        : std::runtime_error(message), status_(status), body_(std::move(body)) {}
    [[nodiscard]] int status() const { return status_; }
    [[nodiscard]] const json& body() const { return body_; }

private:
    int status_;
    json body_;
};

/// ISO 8601 UTC with seconds, "2026-10-18T14:00:00Z".
std::string isoUtc(TimePoint t);
/// Parses "YYYY-MM-DDTHH:MM[:SS[.fff]](Z|±HH:MM)" for the years 1900 to 2200. Nothing on malformed
/// input or a year outside that range.
std::optional<TimePoint> parseIsoUtc(const std::string& text);

/// The time source, so lockouts and expiry can be tested without sleeping.
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual TimePoint now() const = 0;
};

class SystemClock final : public IClock {
public:
    [[nodiscard]] TimePoint now() const override { return Clock::now(); }
};

}  // namespace rfmap
