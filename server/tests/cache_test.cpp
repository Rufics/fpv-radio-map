#include "lru.hpp"
#include "result_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

using namespace rfmap;

namespace {

/// The table tier in memory, with the same LRU-trim semantics as the PostgreSQL one.
class MemoryResultTable final : public IResultTable {
public:
    std::optional<json> get(const std::string& hash) override {
        const auto it = rows.find(hash);
        if (it == rows.end()) return std::nullopt;
        it->second.lastHit = ++tick;
        return std::make_optional(it->second.result);
    }
    void put(const std::string& hash, const std::string&, const json& result, std::size_t bytes) override {
        rows[hash] = {result, bytes, ++tick};
    }
    void trim(std::size_t maxRows, std::size_t maxBytes) override {
        std::vector<std::pair<long, std::string>> order;
        order.reserve(rows.size());
        for (const auto& [h, r] : rows) order.emplace_back(r.lastHit, h);
        std::sort(order.rbegin(), order.rend());
        std::size_t n = 0, total = 0;
        for (const auto& [t, h] : order) {
            ++n;
            total += rows[h].bytes;
            if (n > maxRows || total > maxBytes) rows.erase(h);
        }
    }
    void clear() override { rows.clear(); }
    Usage usage() override {
        std::size_t b = 0;
        for (const auto& [h, r] : rows) b += r.bytes;
        return {rows.size(), b};
    }

    struct Row {
        json result;
        std::size_t bytes{};
        long lastHit{};
    };
    std::map<std::string, Row> rows;
    long tick = 0;
};

using Bytes = LruCache<int, std::string>;
Bytes::Sizer bySize() {
    return [](const std::string& s) { return s.size(); };
}

}  // namespace

TEST(LruCacheTest, StaysWithinItsByteLimitEvictingTheLeastRecentlyUsed) {
    Bytes c(10, bySize());
    c.put(1, std::make_shared<const std::string>("aaaa"));
    c.put(2, std::make_shared<const std::string>("bbbb"));
    ASSERT_NE(c.get(1), nullptr);  // 1 is now the most recent
    c.put(3, std::make_shared<const std::string>("cccc"));
    EXPECT_EQ(c.get(2), nullptr);
    EXPECT_NE(c.get(1), nullptr);
    EXPECT_NE(c.get(3), nullptr);
    const CacheStats s = c.stats();
    EXPECT_LE(s.usedBytes, s.limitBytes);
    EXPECT_EQ(s.usedBytes, 8u);
    EXPECT_EQ(s.entries, 2u);
    EXPECT_EQ(s.evictions, 1u);
    EXPECT_EQ(s.hits, 3u);
    EXPECT_EQ(s.misses, 1u);
    EXPECT_DOUBLE_EQ(s.hitRate(), 0.75);
}

TEST(LruCacheTest, ValueLargerThanTheLimitIsNotKeptAndReplacingUpdatesTheSize) {
    Bytes c(5, bySize());
    c.put(1, std::make_shared<const std::string>("toolarge"));
    EXPECT_EQ(c.get(1), nullptr);
    EXPECT_EQ(c.stats().usedBytes, 0u);
    c.put(2, std::make_shared<const std::string>("ab"));
    c.put(2, std::make_shared<const std::string>("abcd"));
    EXPECT_EQ(c.stats().usedBytes, 4u);
    EXPECT_EQ(c.stats().entries, 1u);
}

TEST(LruCacheTest, LoweringTheLimitEvictsAtOnceAndClearEmpties) {
    Bytes c(100, bySize());
    for (int i = 0; i < 10; ++i) c.put(i, std::make_shared<const std::string>(std::string(10, 'x')));
    EXPECT_EQ(c.stats().usedBytes, 100u);
    c.setLimit(35);
    EXPECT_EQ(c.stats().usedBytes, 30u);
    EXPECT_NE(c.get(9), nullptr);  // newest survive
    EXPECT_EQ(c.get(0), nullptr);
    c.clear();
    EXPECT_EQ(c.stats().entries, 0u);
    EXPECT_EQ(c.stats().usedBytes, 0u);
}

TEST(LruCacheTest, EvictedValueStaysValidForItsHolder) {
    Bytes c(4, bySize());
    c.put(1, std::make_shared<const std::string>("abcd"));
    const auto held = c.get(1);
    c.put(2, std::make_shared<const std::string>("efgh"));
    EXPECT_EQ(c.get(1), nullptr);
    EXPECT_EQ(*held, "abcd");
}

TEST(ResultCacheTest, InputHashIsStableAndKeyOrderFree) {
    const json a = json::parse(R"({"b": 1, "a": [1.5, 2]})");
    const json b = json::parse(R"({"a": [1.5, 2], "b": 1})");
    EXPECT_EQ(inputHash(a), inputHash(b));
    EXPECT_NE(inputHash(a), inputHash(json::parse(R"({"a": [1.5, 2.0000000000000004], "b": 1})")));
    EXPECT_EQ(inputHash(a).size(), 64u);
}

TEST(ResultCacheTest, TableTierHoldsRowAndByteLimitsAndPromotesHits) {
    MemoryResultTable table;
    ResultCache cache({64, 3, 1 << 20}, &table);
    for (int i = 0; i < 40; ++i) cache.put("h" + std::to_string(i), "route", {{"i", i}});
    cache.setLimits({64, 3, 1 << 20});
    EXPECT_LE(table.usage().rows, 3u);
    // The memory tier is bounded by bytes: 64 bytes hold only a few results.
    EXPECT_LE(cache.memoryStats().usedBytes, 64u);
    // Newest rows survive in the table; an old one is gone from both tiers.
    cache.clearMemory();
    EXPECT_FALSE(cache.get("h0"));
    const auto hit = cache.get("h39");
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->at("i"), 39);
    EXPECT_EQ(cache.memoryStats().entries, 1u);  // promoted
    const auto t = cache.tableStats();
    EXPECT_EQ(t.hits, 1u);
    EXPECT_EQ(t.misses, 1u);
}

TEST(ResultCacheTest, ByteLimitOfTheTableTier) {
    MemoryResultTable table;
    const json big = {{"payload", std::string(100, 'x')}};
    const std::size_t one = big.dump().size();
    ResultCache cache({1 << 20, 1000, one * 2}, &table);
    for (int i = 0; i < 20; ++i) cache.put("k" + std::to_string(i), "path", big);
    cache.setLimits({1 << 20, 1000, one * 2});
    EXPECT_LE(table.usage().bytes, one * 2);
    EXPECT_EQ(table.usage().rows, 2u);
    cache.clearTable();
    EXPECT_EQ(table.usage().rows, 0u);
}
