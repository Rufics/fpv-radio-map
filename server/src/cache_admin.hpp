#pragma once

#include "common.hpp"
#include "database.hpp"
#include "raster.hpp"
#include "result_cache.hpp"

#include <string>

namespace rfmap {

/// The admin's view of the three cache tiers (terrain blocks, results in memory, results in the
/// database table): their counters, their limits, and the limits' persistence in the settings
/// table so they survive a restart.
class CacheAdmin {
public:
    CacheAdmin(Database& db, BlockCache& blocks, ResultCache& results);

    /// Applies the limits stored at an earlier PUT, if any.
    void loadStoredLimits() const;
    /// GET /admin/cache: one object per tier.
    [[nodiscard]] json tiers() const;
    /// PUT /admin/cache: [{id, limitBytes?, limitRows?}]; HttpError 422 for anything else.
    void setLimits(const json& request) const;
    /// DELETE /admin/cache?tier=: "all" or one tier id; HttpError 422 for an unknown one.
    void clear(const std::string& tier) const;

private:
    struct Limits {
        std::size_t demBlocksBytes = 0;
        ResultCache::Limits results{};
    };
    [[nodiscard]] Limits current() const;
    void apply(const Limits& limits) const;
    void store(const Limits& limits) const;
    [[nodiscard]] static Limits read(Limits limits, const json& request);
    /// `tier[key]` as a limit, or `current` when absent; HttpError 422 unless a non-negative integer.
    [[nodiscard]] static std::size_t limit(const json& tier, const char* key, std::size_t current);

    Database& db_;
    BlockCache& blocks_;
    ResultCache& results_;
};

}  // namespace rfmap
