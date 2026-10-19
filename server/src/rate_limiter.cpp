#include "rate_limiter.hpp"

#include <cmath>

namespace rfmap {

RateLimiter::RateLimiter(Limits limits, const IClock& clock)
    : limits_(std::move(limits)), clock_(clock) {}

void RateLimiter::take(const std::string& cls, const std::string& client) {
    auto it = limits_.find(cls);
    if (it == limits_.end()) it = limits_.find("default");
    if (it == limits_.end()) return;
    const Config::RateLimit& l = it->second;
    const TimePoint now = clock_.now();
    std::lock_guard lock(mutex_);
    // Forget idle clients now and then, so the table does not grow with every address seen.
    if (now - lastSweep_ > std::chrono::minutes(10)) {
        for (auto b = buckets_.begin(); b != buckets_.end();) {
            b = now - b->second.updated > std::chrono::minutes(30) ? buckets_.erase(b) : std::next(b);
        }
        lastSweep_ = now;
    }
    const std::string key = cls + "|" + client;
    auto [b, fresh] = buckets_.try_emplace(key, Bucket{l.burst, now});
    const double perSecond = l.perMinute / 60.0;
    if (!fresh) {
        const double elapsed = std::chrono::duration<double>(now - b->second.updated).count();
        b->second.tokens = std::min(l.burst, b->second.tokens + elapsed * perSecond);
        b->second.updated = now;
    }
    if (b->second.tokens < 1.0) {
        const auto wait = static_cast<long long>(std::ceil((1.0 - b->second.tokens) / perSecond));
        throw HttpError(429, "too many requests; slow down", {{"retryAfterS", std::max(1LL, wait)}});
    }
    b->second.tokens -= 1.0;
}

}  // namespace rfmap
