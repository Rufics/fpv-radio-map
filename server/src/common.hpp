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

/// ISO 8601 times as the API exchanges them: UTC with seconds, "2026-10-18T14:00:00Z".
class UtcTime {
public:
    [[nodiscard]] static std::string format(TimePoint t);
    /// Parses "YYYY-MM-DDTHH:MM[:SS[.fff]](Z|±HH:MM)" for the years 1900 to 2200. Nothing on
    /// malformed input or a year outside that range.
    [[nodiscard]] static std::optional<TimePoint> parse(const std::string& text);

private:
    struct Fields;
    class Cursor;
    static bool parseDate(Cursor& c, Fields& f);
    static bool parseClock(Cursor& c, Fields& f);
    static bool parseZone(Cursor& c, Fields& f);
};

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
