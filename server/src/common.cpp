#include "common.hpp"

#include <cstdio>
#include <ctime>
#include <regex>

namespace rfmap {

std::string isoUtc(TimePoint t) {
    const std::time_t s = Clock::to_time_t(std::chrono::floor<std::chrono::seconds>(t));
    std::tm tm{};
    gmtime_r(&s, &tm);
    char buf[32];
    (void)std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<TimePoint> parseIsoUtc(const std::string& text) {
    static const std::regex re(
        R"(^(\d{4})-(\d{2})-(\d{2})[Tt ](\d{2}):(\d{2})(?::(\d{2})(\.\d+)?)?([Zz]|([+-])(\d{2}):?(\d{2}))$)");
    std::smatch m;
    if (!std::regex_match(text, m, re)) return std::nullopt;
    std::tm tm{};
    tm.tm_year = std::stoi(m[1]) - 1900;
    tm.tm_mon = std::stoi(m[2]) - 1;
    tm.tm_mday = std::stoi(m[3]);
    tm.tm_hour = std::stoi(m[4]);
    tm.tm_min = std::stoi(m[5]);
    tm.tm_sec = m[6].matched ? std::stoi(m[6]) : 0;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
        tm.tm_sec > 60) {
        return std::nullopt;
    }
    const int day = tm.tm_mday;
    const std::time_t base = timegm(&tm);
    if (tm.tm_mday != day) return std::nullopt;  // timegm normalised an impossible date such as 31 Feb
    TimePoint t = Clock::from_time_t(base);
    if (m[7].matched) {
        const double frac = std::stod("0" + m[7].str());
        t += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frac));
    }
    if (m[9].matched) {
        const int sign = m[9] == "+" ? 1 : -1;
        const auto offset = std::chrono::hours(std::stoi(m[10])) + std::chrono::minutes(std::stoi(m[11]));
        t -= sign * offset;
    }
    return t;
}

}  // namespace rfmap
