#pragma once

// Internal counters of the server in the Prometheus text format, for GET /metrics on the
// operations listener (off unless metrics.enabled). Only plain counters and gauges that are cheap to read: a scrape
// must not slow the server it observes, so nothing here queries the database.

#include "lru.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rfmap {

/// A fixed set of worker threads as the metrics see it.
struct PoolStats {
    int threads = 0;
    std::size_t queued = 0;       ///< tasks waiting for a thread
    int busy = 0;                 ///< threads running a task
    std::uint64_t completed = 0;  ///< tasks finished since start
};

/// The PostgreSQL connection pool.
struct DbPoolStats {
    int size = 0;     ///< sessions open now (lent or idle) or being opened
    int maxSize = 0;  ///< the most the pool opens (database.poolSize)
    int idle = 0;
    int waiting = 0;                    ///< callers waiting for a connection
    std::uint64_t leases = 0;           ///< connections lent since start
    double waitSeconds = 0.0;           ///< total time callers waited for one
    std::uint64_t reconnects = 0;       ///< broken sessions closed; replacements open on demand or in upkeep
    std::uint64_t leaseTimeouts = 0;    ///< callers turned away with 503 after the lease timeout
    std::uint64_t opened = 0;           ///< sessions opened since start
    std::uint64_t closed = 0;           ///< sessions closed: idle, too old, retired or broken
    std::uint64_t connectFailures = 0;  ///< sessions the pool tried and failed to open
};

/// One bulkhead (PIN checks, calculations): its slots, who holds and waits for them, who was turned away.
struct GateSample {
    std::string gate;
    int slots = 0;
    int active = 0;
    int waiting = 0;
    std::uint64_t rejected = 0;
};

/// Coverage jobs the server still keeps, by state, and the pools they run on.
struct JobStats {
    std::map<std::string, int, std::less<>> byState;
    std::uint64_t started = 0;
    PoolStats runners;  ///< one thread per running job
    PoolStats cells;    ///< the cell evaluations of all jobs
};

/// One cache tier; `sized` is false for a tier whose size would need a database query.
struct CacheSample {
    std::string tier;
    CacheStats stats;
    bool sized = true;
};

/// What the kernel says about this process; `known` is false where /proc/self is missing.
struct ProcessStats {
    bool known = false;
    double cpuSeconds = 0.0;     ///< user plus system time
    double residentBytes = 0.0;
    double virtualBytes = 0.0;
    int threads = 0;
    int openFds = -1;            ///< -1 when /proc/self/fd cannot be listed
};

/// Reads /proc/self/stat and /proc/self/fd (Linux).
[[nodiscard]] ProcessStats readProcessStats();

/// One line of /proc/<pid>/stat; the name in parentheses may hold spaces.
[[nodiscard]] ProcessStats parseProcStat(std::string_view line, long ticksPerSecond, long pageBytes);

/// Everything but the HTTP layer, read by the application for one scrape.
struct ServerSample {
    std::string version;
    double startTimeSeconds = 0.0;  ///< Unix time the process started
    ProcessStats process;
    std::vector<std::pair<std::string, PoolStats>> pools;
    DbPoolStats db;
    std::vector<GateSample> gates;
    JobStats jobs;
    std::vector<CacheSample> caches;
};

/// Writes the text format (version 0.0.4) one family at a time: HELP, TYPE, then its samples.
class PrometheusText {
public:
    using Labels = std::vector<std::pair<std::string, std::string>>;

    void family(std::string_view name, std::string_view type, std::string_view help);
    void sample(std::string_view name, double value, const Labels& labels = {});
    [[nodiscard]] const std::string& text() const { return out_; }

    /// A label value with backslash, double quote and newline escaped.
    [[nodiscard]] static std::string escape(std::string_view value);

private:
    std::string out_;
};

/// What the HTTP layer does: requests by rate class, method and status, their durations per
/// rate class, and how many requests wait for or hold one of the server's request threads.
class HttpMetrics {
public:
    static constexpr std::array<double, 12> kBuckets{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0};

    void record(std::string_view rateClass, std::string_view method, int status, double seconds);

    // The request thread pool: a request is queued until a thread takes it, then busy.
    void enqueued() { queued_.fetch_add(1); }
    void rejected() { queued_.fetch_sub(1); }
    void started() {
        queued_.fetch_sub(1);
        busy_.fetch_add(1);
    }
    void finished() {
        busy_.fetch_sub(1);
        completed_.fetch_add(1);
    }
    [[nodiscard]] PoolStats pool(int threads) const;

    void write(PrometheusText& out) const;

private:
    struct Histogram {
        std::array<std::uint64_t, kBuckets.size()> buckets{};  ///< not cumulative; summed when written
        std::uint64_t count = 0;
        double sum = 0.0;
    };

    mutable std::mutex mutex_;
    std::map<std::tuple<std::string, std::string, int>, std::uint64_t, std::less<>> requests_;
    std::map<std::string, Histogram, std::less<>> durations_;
    std::atomic<long> queued_{0};
    std::atomic<int> busy_{0};
    std::atomic<std::uint64_t> completed_{0};
};

/// The families of a ServerSample: build, pools, database, jobs and caches.
void writeServerMetrics(PrometheusText& out, const ServerSample& sample);

}  // namespace rfmap
