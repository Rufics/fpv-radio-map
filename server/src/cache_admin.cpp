#include "cache_admin.hpp"

namespace rfmap {

namespace {
constexpr const char* kDemTier = "dem-blocks";
constexpr const char* kMemoryTier = "results-memory";
constexpr const char* kTableTier = "results-table";

json tierJson(const char* id, const char* label, const CacheStats& s) {
    return {{"id", id},         {"label", label},     {"limitBytes", s.limitBytes}, {"usedBytes", s.usedBytes},
            {"entries", s.entries}, {"hits", s.hits}, {"misses", s.misses},         {"evictions", s.evictions},
            {"hitRate", s.hitRate()}};
}
}  // namespace

CacheAdmin::CacheAdmin(Database& db, BlockCache& blocks, ResultCache& results)
    : db_(db), blocks_(blocks), results_(results) {}

CacheAdmin::Limits CacheAdmin::current() const { return {blocks_.stats().limitBytes, results_.limits()}; }

void CacheAdmin::apply(const Limits& limits) const {
    blocks_.setLimit(limits.demBlocksBytes);
    results_.setLimits(limits.results);
}

void CacheAdmin::loadStoredLimits() const {
    const auto stored = db_.tx([](pqxx::work& w) -> std::optional<json> {
        const auto r = w.exec("select value::text from settings where key = 'cache.limits'");
        if (r.empty()) return std::nullopt;
        return std::make_optional(json::parse(r[0][0].as<std::string>()));
    });
    if (!stored) return;
    Limits l = current();
    l.demBlocksBytes = stored->value("demBlocksBytes", l.demBlocksBytes);
    l.results.memoryBytes = stored->value("resultMemoryBytes", l.results.memoryBytes);
    l.results.tableRows = stored->value("resultTableRows", l.results.tableRows);
    l.results.tableBytes = stored->value("resultTableBytes", l.results.tableBytes);
    apply(l);
}

void CacheAdmin::store(const Limits& l) const {
    const json stored = {{"demBlocksBytes", l.demBlocksBytes},
                         {"resultMemoryBytes", l.results.memoryBytes},
                         {"resultTableRows", l.results.tableRows},
                         {"resultTableBytes", l.results.tableBytes}};
    db_.tx([&stored](pqxx::work& w) {
        w.exec("insert into settings(key, value) values ('cache.limits', $1::jsonb) "
               "on conflict (key) do update set value = excluded.value",
               pqxx::params{stored.dump()});
    });
}

json CacheAdmin::tiers() const {
    const auto table = results_.tableStats();
    const auto limits = results_.limits();
    const auto rate = [](std::size_t h, std::size_t m) {
        return h + m == 0 ? 0.0 : static_cast<double>(h) / static_cast<double>(h + m);
    };
    return json::array({tierJson(kDemTier, "Terrain and land cover blocks (memory)", blocks_.stats()),
                        tierJson(kMemoryTier, "Results (memory)", results_.memoryStats()),
                        {{"id", kTableTier}, {"label", "Results (database table)"}, {"limitBytes", limits.tableBytes},
                         {"usedBytes", table.bytes}, {"limitRows", limits.tableRows}, {"entries", table.rows},
                         {"hits", table.hits}, {"misses", table.misses}, {"hitRate", rate(table.hits, table.misses)}}});
}

CacheAdmin::Limits CacheAdmin::read(Limits l, const json& request) {
    if (!request.is_array()) throw HttpError(422, "send [{id, limitBytes?, limitRows?}]");
    for (const json& t : request) {
        if (!t.is_object() || !t.contains("id") || !t.at("id").is_string()) throw HttpError(422, "each tier needs an id");
        const std::string id = t.at("id");
        if (id == kDemTier) {
            l.demBlocksBytes = limit(t, "limitBytes", l.demBlocksBytes);
        } else if (id == kMemoryTier) {
            l.results.memoryBytes = limit(t, "limitBytes", l.results.memoryBytes);
        } else if (id == kTableTier) {
            l.results.tableBytes = limit(t, "limitBytes", l.results.tableBytes);
            l.results.tableRows = limit(t, "limitRows", l.results.tableRows);
        } else {
            throw HttpError(422, "unknown cache tier " + id);
        }
    }
    return l;
}

std::size_t CacheAdmin::limit(const json& tier, const char* key, std::size_t current) {
    if (!tier.contains(key)) return current;
    const json& v = tier.at(key);
    // A negative or fractional number would wrap into an unbounded cache and be stored as such.
    if (!v.is_number_unsigned()) throw HttpError(422, std::string(key) + " must be a non-negative integer");
    return v.get<std::size_t>();
}

void CacheAdmin::setLimits(const json& request) const {
    const Limits l = read(current(), request);
    apply(l);
    store(l);
}

void CacheAdmin::clear(const std::string& tier) const {
    const bool all = tier == "all";
    if (!all && tier != kDemTier && tier != kMemoryTier && tier != kTableTier) {
        throw HttpError(422, "unknown cache tier " + tier);
    }
    if (all || tier == kDemTier) blocks_.clear();
    if (all || tier == kMemoryTier) results_.clearMemory();
    if (all || tier == kTableTier) results_.clearTable();
}

}  // namespace rfmap
