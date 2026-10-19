#pragma once

#include "common.hpp"
#include "config.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rfmap {

/// Token buckets per (class, client address). A request takes one token; an empty bucket
/// answers 429 with the seconds until the next token.
class RateLimiter {
public:
    using Limits = std::map<std::string, Config::RateLimit, std::less<>>;
    RateLimiter(Limits limits, const IClock& clock);

    /// Throws HttpError 429 {retryAfterS} when the client is over the class's rate.
    void take(const std::string& cls, const std::string& client);

private:
    struct Bucket {
        double tokens{};
        TimePoint updated;
    };
    /// Hashes std::string and std::string_view alike, so lookups need not build a key string.
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    Limits limits_;
    const IClock& clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket, KeyHash, std::equal_to<>> buckets_;
    TimePoint lastSweep_{};
};

}  // namespace rfmap
