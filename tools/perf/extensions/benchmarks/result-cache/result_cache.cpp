// The result cache sits in front of every calculation. A hit must stay far cheaper than the
// calculation, also when many request threads share the cache.

#include "lru.hpp"
#include "result_cache.hpp"

#include <benchmark/benchmark.h>

#include <map>
#include <mutex>
#include <string>

namespace {

using rfmap::json;

/// The table tier in memory, so the benchmark measures the cache and not PostgreSQL.
class MemoryResultTable final : public rfmap::IResultTable {
public:
    std::optional<json> get(const std::string& hash) override {
        std::lock_guard lock(mutex_);
        const auto it = rows_.find(hash);
        if (it == rows_.end()) return std::nullopt;
        return it->second;
    }
    void put(const std::string& hash, const std::string&, const json& result, std::size_t) override {
        std::lock_guard lock(mutex_);
        rows_[hash] = result;
    }
    void trim(std::size_t, std::size_t) override {}
    void clear() override {
        std::lock_guard lock(mutex_);
        rows_.clear();
    }
    Usage usage() override {
        std::lock_guard lock(mutex_);
        return {rows_.size(), 0};
    }

private:
    std::mutex mutex_;
    std::map<std::string, json> rows_;
};

/// About the size of a path result: a profile of `points` samples plus the budget terms.
json pathResult(int points) {
    json profile = json::array();
    for (int i = 0; i < points; ++i) {
        profile.push_back({{"d", i * 30.0}, {"ground", 110.0 + i % 17}, {"clearance", 0.6 + i % 5 * 0.1}});
    }
    return {{"kind", "path"},
            {"marginDb", 11.4},
            {"terms", json::array({{{"name", "fspl"}, {"db", -128.9}}, {{"name", "diffraction"}, {"db", -6.2}}})},
            {"profile", std::move(profile)}};
}

json request(int seed) {
    return {{"tx", {{"lat", 52.2297}, {"lon", 21.0122 + seed * 1e-4}, {"aglM", 30}}},
            {"rx", {{"lat", 52.3100}, {"lon", 21.2400}, {"aglM", 2}}},
            {"frequencyHz", 5.8e9},
            {"versions", {{"server", "0.1.0"}, {"dem", "glo30-2023_1"}, {"modules", {{"rf.clutter", "1.2.0"}}}}}};
}

constexpr rfmap::ResultCache::Limits kLimits{256u << 20, 100'000, 1u << 30};

void BM_InputHash(benchmark::State& state) {
    const json inputs = request(1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(rfmap::inputHash(inputs));
    }
}
BENCHMARK(BM_InputHash);

// A memory hit parses the stored JSON. Threads share one cache, as the request threads do.
void BM_MemoryHit(benchmark::State& state) {
    static MemoryResultTable table;
    static rfmap::ResultCache cache(kLimits, &table);
    const std::string hash = rfmap::inputHash(request(static_cast<int>(state.range(0))));
    if (state.thread_index() == 0) cache.put(hash, "path", pathResult(static_cast<int>(state.range(0))));
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.get(hash));
    }
}
BENCHMARK(BM_MemoryHit)->Arg(400)->Arg(1700)->ThreadRange(1, 8)->UseRealTime();

// Memory was cleared (a restart): the row comes from the table and is promoted.
void BM_TablePromote(benchmark::State& state) {
    MemoryResultTable table;
    rfmap::ResultCache cache(kLimits, &table);
    const std::string hash = rfmap::inputHash(request(1));
    cache.put(hash, "path", pathResult(static_cast<int>(state.range(0))));
    for (auto _ : state) {
        state.PauseTiming();
        cache.clearMemory();
        state.ResumeTiming();
        benchmark::DoNotOptimize(cache.get(hash));
    }
}
BENCHMARK(BM_TablePromote)->Arg(400)->Arg(1700);

// New results into a full memory tier: every put evicts the least recently used one.
void BM_PutWithEviction(benchmark::State& state) {
    const json result = pathResult(400);
    rfmap::ResultCache cache({result.dump().size() * 64, 0, 0}, nullptr);
    int seed = 0;
    for (auto _ : state) {
        state.PauseTiming();
        const std::string hash = rfmap::inputHash(request(++seed));
        state.ResumeTiming();
        cache.put(hash, "path", result);
    }
}
BENCHMARK(BM_PutWithEviction);

}  // namespace
