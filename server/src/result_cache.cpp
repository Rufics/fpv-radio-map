#include "result_cache.hpp"

#include "security.hpp"

namespace rfmap {

std::string inputHash(const json& inputs) { return Secrets::hex(Secrets::sha256(inputs.dump())); }

std::optional<json> PgResultTable::get(const std::string& hash) {
    return db_.tx([&hash](pqxx::work& w) -> std::optional<json> {
        const auto r = w.exec_params(
            "update result_cache set last_hit_at = now(), hits = hits + 1 where input_hash = $1 returning result::text",
            hash);
        if (r.empty()) return std::nullopt;
        return json::parse(r[0][0].as<std::string>());
    });
}

void PgResultTable::put(const std::string& hash, const std::string& kind, const json& result, std::size_t bytes) {
    db_.tx([&hash, &kind, &result, bytes](pqxx::work& w) {
        w.exec_params(
            "insert into result_cache(input_hash, kind, result, bytes) values ($1, $2, $3::jsonb, $4) "
            "on conflict (input_hash) do update set result = excluded.result, bytes = excluded.bytes, last_hit_at = now()",
            hash, kind, result.dump(), static_cast<long long>(bytes));
    });
}

void PgResultTable::trim(std::size_t maxRows, std::size_t maxBytes) {
    db_.tx([maxRows, maxBytes](pqxx::work& w) {
        // Keep the newest rows whose running byte total stays within the limit.
        w.exec_params(
            "delete from result_cache where input_hash in ("
            "  select input_hash from ("
            "    select input_hash, row_number() over (order by last_hit_at desc) as n,"
            "           sum(bytes) over (order by last_hit_at desc rows unbounded preceding) as total"
            "    from result_cache) ranked"
            "  where n > $1 or total > $2)",
            static_cast<long long>(maxRows), static_cast<long long>(maxBytes));
    });
}

void PgResultTable::clear() {
    db_.tx([](pqxx::work& w) { w.exec("delete from result_cache"); });
}

IResultTable::Usage PgResultTable::usage() {
    return db_.tx([](pqxx::work& w) {
        const auto r = w.exec("select count(*), coalesce(sum(bytes), 0) from result_cache");
        return Usage{r[0][0].as<std::size_t>(), r[0][1].as<std::size_t>()};
    });
}

ResultCache::ResultCache(Limits limits, IResultTable* table)
    : memory_(limits.memoryBytes, [](const std::string& s) { return s.size(); }), table_(table), limits_(limits) {}

std::optional<json> ResultCache::get(const std::string& hash) {
    if (auto hit = memory_.get(hash)) return json::parse(*hit);
    if (!table_) return std::nullopt;
    auto row = table_->get(hash);
    {
        std::lock_guard lock(mutex_);
        ++(row ? tableHits_ : tableMisses_);
    }
    if (row) memory_.put(hash, std::make_shared<const std::string>(row->dump()));
    return row;
}

void ResultCache::put(const std::string& hash, const std::string& kind, const json& result) {
    auto text = std::make_shared<const std::string>(result.dump());
    const std::size_t bytes = text->size();
    memory_.put(hash, text);
    if (!table_) return;
    Limits l{};
    bool trimNow = false;
    {
        std::lock_guard lock(mutex_);
        l = limits_;
        // Trimming scans the table, so do it every few writes rather than on each one.
        trimNow = ++putsSinceTrim_ >= 16 || bytes > l.tableBytes / 16;
        if (trimNow) putsSinceTrim_ = 0;
    }
    if (bytes > l.tableBytes || l.tableRows == 0) return;
    table_->put(hash, kind, result, bytes);
    if (trimNow) table_->trim(l.tableRows, l.tableBytes);
}

void ResultCache::setLimits(Limits limits) {
    {
        std::lock_guard lock(mutex_);
        limits_ = limits;
    }
    memory_.setLimit(limits.memoryBytes);
    if (table_) table_->trim(limits.tableRows, limits.tableBytes);
}

ResultCache::Limits ResultCache::limits() const {
    std::lock_guard lock(mutex_);
    return limits_;
}

void ResultCache::clearMemory() { memory_.clear(); }

void ResultCache::clearTable() {
    if (table_) table_->clear();
}

ResultCache::TableStats ResultCache::tableStats() const {
    const IResultTable::Usage u = table_ ? table_->usage() : IResultTable::Usage{0, 0};
    std::lock_guard lock(mutex_);
    return {u.rows, u.bytes, tableHits_, tableMisses_};
}

}  // namespace rfmap
