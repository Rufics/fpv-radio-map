#include "common.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <string_view>

namespace rfmap {

std::string UtcTime::format(TimePoint t) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(t));
}

namespace {

constexpr int kMinYear = 1900;
constexpr int kMaxYear = 2200;
constexpr std::time_t kFirstSecond = -2208988800;     // 1900-01-01T00:00:00Z
constexpr std::time_t kAfterLastSecond = 7289654400;  // 2201-01-01T00:00:00Z

}  // namespace

/// A cursor over the text being parsed.
class UtcTime::Cursor {
public:
    explicit Cursor(std::string_view text) : s(text) {}

    /// Reads exactly `n` ASCII digits.
    bool digits(std::size_t n, int& out) {
        if (pos + n > s.size()) return false;
        out = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        pos += n;
        return true;
    }
    bool literal(char c) {
        if (pos >= s.size() || s[pos] != c) return false;
        ++pos;
        return true;
    }
    bool oneOf(std::string_view chars) {
        if (pos >= s.size() || std::ranges::find(chars, s[pos]) == chars.end()) return false;
        ++pos;
        return true;
    }
    [[nodiscard]] bool done() const { return pos == s.size(); }
    [[nodiscard]] std::size_t position() const { return pos; }
    /// The character just consumed.
    [[nodiscard]] char previous() const { return s[pos - 1]; }

private:
    std::string_view s;
    std::size_t pos = 0;
};

struct UtcTime::Fields {
    std::tm tm{};
    double fraction = 0.0;
    std::chrono::minutes offset{0};
};

namespace {

bool inRange(const std::tm& tm) {
    return tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_mday >= 1 && tm.tm_mday <= 31 && tm.tm_hour <= 23 &&
           tm.tm_min <= 59 && tm.tm_sec <= 60;
}

}  // namespace

/// YYYY-MM-DD
bool UtcTime::parseDate(Cursor& c, Fields& f) {
    int year = 0;
    int month = 0;
    if (!c.digits(4, year) || !c.literal('-') || !c.digits(2, month) || !c.literal('-') || !c.digits(2, f.tm.tm_mday)) {
        return false;
    }
    f.tm.tm_year = year - 1900;
    f.tm.tm_mon = month - 1;
    return year >= kMinYear && year <= kMaxYear;
}

/// HH:MM[:SS[.fff]], the fraction of any length.
bool UtcTime::parseClock(Cursor& c, Fields& f) {
    if (!c.digits(2, f.tm.tm_hour) || !c.literal(':') || !c.digits(2, f.tm.tm_min)) return false;
    if (!c.literal(':')) return true;
    if (!c.digits(2, f.tm.tm_sec)) return false;
    if (!c.literal('.')) return true;
    const std::size_t from = c.position();
    double scale = 0.1;
    int d = 0;
    while (c.digits(1, d)) {
        f.fraction += d * scale;
        scale /= 10.0;
    }
    return c.position() > from;
}

/// Z or ±HH[:]MM
bool UtcTime::parseZone(Cursor& c, Fields& f) {
    if (c.oneOf("Zz")) return true;
    if (!c.oneOf("+-")) return false;
    const int sign = c.previous() == '+' ? 1 : -1;
    int hours = 0;
    int minutes = 0;
    if (!c.digits(2, hours)) return false;
    c.literal(':');
    if (!c.digits(2, minutes)) return false;
    f.offset = sign * (std::chrono::hours(hours) + std::chrono::minutes(minutes));
    return true;
}

// Parsed by hand: std::regex matches recursively, one stack frame per character, so a long
// fraction of a second in a request body would overflow the stack.
std::optional<TimePoint> UtcTime::parse(const std::string& text) {
    Cursor c(text);
    Fields f;
    if (!parseDate(c, f) || !c.oneOf("Tt ") || !parseClock(c, f) || !parseZone(c, f) || !c.done() ||
        !inRange(f.tm)) {
        return std::nullopt;
    }
    const int day = f.tm.tm_mday;
    const std::time_t base = timegm(&f.tm);
    if (f.tm.tm_mday != day) return std::nullopt;  // timegm normalised an impossible date such as 31 Feb
    const TimePoint t = Clock::from_time_t(base) +
                        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(f.fraction)) -
                        f.offset;
    // The offset can move the time out of the range; outside it, format() would print a year the
    // parser refuses. The clock counts nanoseconds in 64 bits, which reaches 1678 to 2262 only.
    if (t < Clock::from_time_t(kFirstSecond) || t >= Clock::from_time_t(kAfterLastSecond)) return std::nullopt;
    return t;
}

}  // namespace rfmap
