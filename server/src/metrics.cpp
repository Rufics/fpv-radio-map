#include "metrics.hpp"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

namespace rfmap {

void PrometheusText::family(std::string_view name, std::string_view type, std::string_view help) {
    out_ += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

void PrometheusText::sample(std::string_view name, double value, const Labels& labels) {
    out_ += name;
    if (!labels.empty()) {
        out_ += '{';
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (i > 0) out_ += ',';
            out_ += std::format("{}=\"{}\"", labels[i].first, escape(labels[i].second));
        }
        out_ += '}';
    }
    out_ += std::format(" {}\n", value);
}

std::string PrometheusText::escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void HttpMetrics::record(std::string_view rateClass, std::string_view method, int status, double seconds) {
    std::lock_guard lock(mutex_);
    ++requests_[{std::string(rateClass), std::string(method), status}];
    auto it = durations_.find(rateClass);
    if (it == durations_.end()) it = durations_.emplace(std::string(rateClass), Histogram{}).first;
    Histogram& h = it->second;
    for (std::size_t i = 0; i < kBuckets.size(); ++i) {
        if (seconds <= kBuckets[i]) {
            ++h.buckets[i];
            break;
        }
    }
    ++h.count;
    h.sum += seconds;
}

PoolStats HttpMetrics::pool(int threads) const {
    return {threads, static_cast<std::size_t>(std::max(0L, queued_.load(std::memory_order_relaxed))),
            busy_.load(std::memory_order_relaxed), completed_.load(std::memory_order_relaxed)};
}

void HttpMetrics::write(PrometheusText& out) const {
    std::lock_guard lock(mutex_);
    out.family("rfmap_http_requests_total", "counter", "Answered requests by rate class, method and status.");
    for (const auto& [key, count] : requests_) {
        const auto& [rateClass, method, status] = key;
        out.sample("rfmap_http_requests_total", static_cast<double>(count),
                   {{"class", rateClass}, {"method", method}, {"status", std::to_string(status)}});
    }
    out.family("rfmap_http_request_duration_seconds", "histogram", "Time from routing to the answer, by rate class.");
    for (const auto& [rateClass, h] : durations_) {
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kBuckets.size(); ++i) {
            cumulative += h.buckets[i];
            out.sample("rfmap_http_request_duration_seconds_bucket", static_cast<double>(cumulative),
                       {{"class", rateClass}, {"le", std::format("{}", kBuckets[i])}});
        }
        out.sample("rfmap_http_request_duration_seconds_bucket", static_cast<double>(h.count),
                   {{"class", rateClass}, {"le", "+Inf"}});
        out.sample("rfmap_http_request_duration_seconds_sum", h.sum, {{"class", rateClass}});
        out.sample("rfmap_http_request_duration_seconds_count", static_cast<double>(h.count), {{"class", rateClass}});
    }
}

namespace {

using Labels = PrometheusText::Labels;

void writePools(PrometheusText& out, const std::vector<std::pair<std::string, PoolStats>>& pools) {
    const auto each = [&](std::string_view name, std::string_view type, std::string_view help, auto value) {
        out.family(name, type, help);
        for (const auto& [pool, s] : pools) out.sample(name, static_cast<double>(value(s)), {{"pool", pool}});
    };
    each("rfmap_pool_threads", "gauge", "Worker threads of the pool.", [](const PoolStats& s) { return s.threads; });
    each("rfmap_pool_busy_threads", "gauge", "Threads running a task.", [](const PoolStats& s) { return s.busy; });
    each("rfmap_pool_queued_tasks", "gauge", "Tasks waiting for a thread.", [](const PoolStats& s) { return s.queued; });
    each("rfmap_pool_tasks_completed_total", "counter", "Tasks finished since start.",
         [](const PoolStats& s) { return s.completed; });
}

void writeDatabase(PrometheusText& out, const DbPoolStats& db) {
    const auto one = [&](std::string_view name, std::string_view type, std::string_view help, double value) {
        out.family(name, type, help);
        out.sample(name, value);
    };
    one("rfmap_db_pool_connections", "gauge", "Connections in the PostgreSQL pool.", db.size);
    one("rfmap_db_pool_idle_connections", "gauge", "Connections not lent out.", db.idle);
    one("rfmap_db_pool_waiting", "gauge", "Callers waiting for a connection.", db.waiting);
    one("rfmap_db_pool_leases_total", "counter", "Connections lent since start.", static_cast<double>(db.leases));
    one("rfmap_db_pool_wait_seconds_total", "counter", "Time callers waited for a connection.", db.waitSeconds);
    one("rfmap_db_pool_reconnects_total", "counter", "Broken connections replaced.", static_cast<double>(db.reconnects));
}

void writeJobs(PrometheusText& out, const JobStats& jobs) {
    out.family("rfmap_jobs", "gauge", "Coverage jobs the server keeps, by state.");
    for (const auto& [state, n] : jobs.byState) out.sample("rfmap_jobs", n, {{"state", state}});
    out.family("rfmap_jobs_started_total", "counter", "Coverage jobs started since start.");
    out.sample("rfmap_jobs_started_total", static_cast<double>(jobs.started));
}

void writeCaches(PrometheusText& out, const std::vector<CacheSample>& caches) {
    const auto each = [&](std::string_view name, std::string_view type, std::string_view help, bool sizedOnly,
                          auto value) {
        out.family(name, type, help);
        for (const CacheSample& c : caches) {
            if (c.sized || !sizedOnly) out.sample(name, static_cast<double>(value(c.stats)), {{"tier", c.tier}});
        }
    };
    each("rfmap_cache_hits_total", "counter", "Lookups answered by the tier.", false,
         [](const CacheStats& s) { return s.hits; });
    each("rfmap_cache_misses_total", "counter", "Lookups the tier could not answer.", false,
         [](const CacheStats& s) { return s.misses; });
    each("rfmap_cache_evictions_total", "counter", "Entries dropped to stay within the limit.", true,
         [](const CacheStats& s) { return s.evictions; });
    each("rfmap_cache_entries", "gauge", "Entries held.", true, [](const CacheStats& s) { return s.entries; });
    each("rfmap_cache_used_bytes", "gauge", "Bytes held.", true, [](const CacheStats& s) { return s.usedBytes; });
    each("rfmap_cache_limit_bytes", "gauge", "Byte limit of the tier.", true,
         [](const CacheStats& s) { return s.limitBytes; });
}

void writeProcess(PrometheusText& out, const ProcessStats& p) {
    if (!p.known) return;
    out.family("rfmap_process_cpu_seconds_total", "counter", "User and system CPU time of the server.");
    out.sample("rfmap_process_cpu_seconds_total", p.cpuSeconds);
    out.family("rfmap_process_resident_memory_bytes", "gauge", "Resident memory of the server.");
    out.sample("rfmap_process_resident_memory_bytes", p.residentBytes);
    out.family("rfmap_process_virtual_memory_bytes", "gauge", "Virtual memory of the server.");
    out.sample("rfmap_process_virtual_memory_bytes", p.virtualBytes);
    out.family("rfmap_process_threads", "gauge", "Threads of the server.");
    out.sample("rfmap_process_threads", p.threads);
    if (p.openFds >= 0) {
        out.family("rfmap_process_open_fds", "gauge", "Open file descriptors of the server.");
        out.sample("rfmap_process_open_fds", p.openFds);
    }
}

// Fields of /proc/<pid>/stat counted from the one after the name: state is 0, utime 11.
constexpr std::size_t kUtime = 11;
constexpr std::size_t kStime = 12;
constexpr std::size_t kThreads = 17;
constexpr std::size_t kVsize = 20;
constexpr std::size_t kRss = 21;

double field(const std::vector<std::string_view>& fields, std::size_t index) {
    double value = 0.0;
    const std::string_view text = fields[index];
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

}  // namespace

ProcessStats parseProcStat(std::string_view line, long ticksPerSecond, long pageBytes) {
    ProcessStats stats;
    const auto close = line.rfind(')');
    if (close == std::string_view::npos || ticksPerSecond <= 0) return stats;
    std::vector<std::string_view> fields;
    std::string_view rest = line.substr(close + 1);
    while (!rest.empty()) {
        const auto start = rest.find_first_not_of(" \n");
        if (start == std::string_view::npos) break;
        rest.remove_prefix(start);
        const auto end = std::min(rest.find_first_of(" \n"), rest.size());
        fields.push_back(rest.substr(0, end));
        rest.remove_prefix(end);
    }
    if (fields.size() <= kRss) return stats;
    stats.known = true;
    stats.cpuSeconds = (field(fields, kUtime) + field(fields, kStime)) / static_cast<double>(ticksPerSecond);
    stats.threads = static_cast<int>(field(fields, kThreads));
    stats.virtualBytes = field(fields, kVsize);
    stats.residentBytes = field(fields, kRss) * static_cast<double>(pageBytes);
    return stats;
}

ProcessStats readProcessStats() {
    std::ifstream in("/proc/self/stat");
    const std::string line{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ProcessStats stats = parseProcStat(line, sysconf(_SC_CLK_TCK), sysconf(_SC_PAGESIZE));
    std::error_code error;
    std::filesystem::directory_iterator fds("/proc/self/fd", error);
    if (!error) stats.openFds = static_cast<int>(std::distance(fds, std::filesystem::directory_iterator{}));
    return stats;
}

void writeServerMetrics(PrometheusText& out, const ServerSample& sample) {
    out.family("rfmap_build_info", "gauge", "Always 1; the server version is a label.");
    out.sample("rfmap_build_info", 1, {{"version", sample.version}});
    out.family("rfmap_process_start_time_seconds", "gauge", "Unix time the server started.");
    out.sample("rfmap_process_start_time_seconds", sample.startTimeSeconds);
    writeProcess(out, sample.process);
    writePools(out, sample.pools);
    writeDatabase(out, sample.db);
    writeJobs(out, sample.jobs);
    writeCaches(out, sample.caches);
}

}  // namespace rfmap
