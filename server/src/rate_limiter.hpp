#pragma once

#include "common.hpp"
#include "config.hpp"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rfmap {

/// Token buckets per (class, client address). A request takes one token; an empty bucket
/// answers 429 with the seconds until the next token.
class RateLimiter {
public:
    RateLimiter(std::map<std::string, Config::RateLimit> limits, const IClock& clock);

    /// Throws HttpError 429 {retryAfterS} when the client is over the class's rate.
    void take(const std::string& cls, const std::string& client);

private:
    struct Bucket {
        double tokens{};
        TimePoint updated;
    };
    std::map<std::string, Config::RateLimit> limits_;
    const IClock& clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    TimePoint lastSweep_{};
};

}  // namespace rfmap
