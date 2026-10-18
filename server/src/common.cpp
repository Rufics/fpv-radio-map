#include "common.hpp"

#include <cstdio>
#include <ctime>

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

/// Reads exactly `n` ASCII digits at `pos`.
bool digits(const std::string& s, std::size_t& pos, std::size_t n, int& out) {
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

constexpr int kMinYear = 1900;
constexpr int kMaxYear = 2200;
constexpr std::time_t kFirstSecond = -2208988800;     // 1900-01-01T00:00:00Z
constexpr std::time_t kAfterLastSecond = 7289654400;  // 2201-01-01T00:00:00Z

bool literal(const std::string& s, std::size_t& pos, char c) {
    if (pos >= s.size() || s[pos] != c) return false;
    ++pos;
    return true;
}

}  // namespace

// Parsed by hand: std::regex matches recursively, one stack frame per character, so a long
// fraction of a second in a request body would overflow the stack.
std::optional<TimePoint> parseIsoUtc(const std::string& text) {
    std::size_t pos = 0;
    std::tm tm{};
    int year = 0, month = 0, day = 0;
    if (!digits(text, pos, 4, year) || !literal(text, pos, '-') || !digits(text, pos, 2, month) ||
        !literal(text, pos, '-') || !digits(text, pos, 2, day)) {
        return std::nullopt;
    }
    if (pos >= text.size() || (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ')) return std::nullopt;
    ++pos;
    if (!digits(text, pos, 2, tm.tm_hour) || !literal(text, pos, ':') || !digits(text, pos, 2, tm.tm_min)) {
        return std::nullopt;
    }
    double frac = 0.0;
    if (literal(text, pos, ':')) {
        if (!digits(text, pos, 2, tm.tm_sec)) return std::nullopt;
        if (literal(text, pos, '.')) {
            const std::size_t from = pos;
            double scale = 0.1;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                frac += (text[pos] - '0') * scale;
                scale /= 10.0;
                ++pos;
            }
            if (pos == from) return std::nullopt;
        }
    }
    int sign = 0, offH = 0, offM = 0;
    const bool utc = literal(text, pos, 'Z') || literal(text, pos, 'z');
    if (!utc) {
        if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) return std::nullopt;
        sign = text[pos] == '+' ? 1 : -1;
        ++pos;
        if (!digits(text, pos, 2, offH)) return std::nullopt;
        literal(text, pos, ':');
        if (!digits(text, pos, 2, offM)) return std::nullopt;
    }
    if (pos != text.size()) return std::nullopt;

    // The clock counts nanoseconds in 64 bits, which reaches the years 1678 to 2262 only.
    if (year < kMinYear || year > kMaxYear) return std::nullopt;
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
        tm.tm_sec > 60) {
        return std::nullopt;
    }
    const std::time_t base = timegm(&tm);
    if (tm.tm_mday != day) return std::nullopt;  // timegm normalised an impossible date such as 31 Feb
    TimePoint t = Clock::from_time_t(base);
    t += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frac));
    t -= sign * (std::chrono::hours(offH) + std::chrono::minutes(offM));
    // The offset can move the time out of the range; outside it, isoUtc would print a year the
    // parser refuses.
    if (t < Clock::from_time_t(kFirstSecond) || t >= Clock::from_time_t(kAfterLastSecond)) return std::nullopt;
    return t;
}

}  // namespace rfmap
