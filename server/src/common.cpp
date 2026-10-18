#include "common.hpp"

#include <algorithm>
#include <ctime>
#include <string_view>

namespace rfmap {

std::string isoUtc(TimePoint t) {
    const std::time_t s = Clock::to_time_t(std::chrono::floor<std::chrono::seconds>(t));
    std::tm tm{};
    gmtime_r(&s, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

namespace {

constexpr int kMinYear = 1900;
constexpr int kMaxYear = 2200;
constexpr std::time_t kFirstSecond = -2208988800;     // 1900-01-01T00:00:00Z
constexpr std::time_t kAfterLastSecond = 7289654400;  // 2201-01-01T00:00:00Z

/// A cursor over the text being parsed.
struct Cursor {
    std::string_view s;
    std::size_t pos = 0;

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
};

struct IsoFields {
    std::tm tm{};
    double fraction = 0.0;
    std::chrono::minutes offset{0};
};

/// YYYY-MM-DD
bool parseDate(Cursor& c, IsoFields& f) {
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
bool parseClock(Cursor& c, IsoFields& f) {
    if (!c.digits(2, f.tm.tm_hour) || !c.literal(':') || !c.digits(2, f.tm.tm_min)) return false;
    if (!c.literal(':')) return true;
    if (!c.digits(2, f.tm.tm_sec)) return false;
    if (!c.literal('.')) return true;
    const std::size_t from = c.pos;
    double scale = 0.1;
    int d = 0;
    while (c.digits(1, d)) {
        f.fraction += d * scale;
        scale /= 10.0;
    }
    return c.pos > from;
}

/// Z or ±HH[:]MM
bool parseZone(Cursor& c, IsoFields& f) {
    if (c.oneOf("Zz")) return true;
    if (!c.oneOf("+-")) return false;
    const int sign = c.s[c.pos - 1] == '+' ? 1 : -1;
    int hours = 0;
    int minutes = 0;
    if (!c.digits(2, hours)) return false;
    c.literal(':');
    if (!c.digits(2, minutes)) return false;
    f.offset = sign * (std::chrono::hours(hours) + std::chrono::minutes(minutes));
    return true;
}

bool inRange(const std::tm& tm) {
    return tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_mday >= 1 && tm.tm_mday <= 31 && tm.tm_hour <= 23 &&
           tm.tm_min <= 59 && tm.tm_sec <= 60;
}

}  // namespace

// Parsed by hand: std::regex matches recursively, one stack frame per character, so a long
// fraction of a second in a request body would overflow the stack.
std::optional<TimePoint> parseIsoUtc(const std::string& text) {
    Cursor c{text};
    IsoFields f;
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
    // The offset can move the time out of the range; outside it, isoUtc would print a year the
    // parser refuses. The clock counts nanoseconds in 64 bits, which reaches 1678 to 2262 only.
    if (t < Clock::from_time_t(kFirstSecond) || t >= Clock::from_time_t(kAfterLastSecond)) return std::nullopt;
    return t;
}

}  // namespace rfmap
