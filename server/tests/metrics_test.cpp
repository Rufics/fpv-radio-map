#include "metrics.hpp"
#include "thread_pool.hpp"

#include <gtest/gtest.h>

#include <future>
#include <latch>
#include <string>

using namespace rfmap;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(PrometheusTextTest, FamiliesSamplesAndEscapedLabels) {
    PrometheusText out;
    out.family("rfmap_x", "gauge", "An example.");
    out.sample("rfmap_x", 2.5);
    out.sample("rfmap_x", 3, {{"a", "q\"uote\\back\nline"}, {"b", "2"}});
    EXPECT_EQ(out.text(),
              "# HELP rfmap_x An example.\n# TYPE rfmap_x gauge\nrfmap_x 2.5\n"
              "rfmap_x{a=\"q\\\"uote\\\\back\\nline\",b=\"2\"} 3\n");
}

TEST(HttpMetricsTest, CountsRequestsAndCumulativeBuckets) {
    HttpMetrics m;
    m.record("calc", "POST", 200, 0.004);
    m.record("calc", "POST", 200, 0.3);
    m.record("calc", "POST", 429, 60.0);
    m.record("default", "GET", 404, 0.02);
    PrometheusText out;
    m.write(out);
    const std::string& t = out.text();
    EXPECT_TRUE(contains(t, "rfmap_http_requests_total{class=\"calc\",method=\"POST\",status=\"200\"} 2\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_requests_total{class=\"calc\",method=\"POST\",status=\"429\"} 1\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_request_duration_seconds_bucket{class=\"calc\",le=\"0.005\"} 1\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_request_duration_seconds_bucket{class=\"calc\",le=\"0.5\"} 2\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_request_duration_seconds_bucket{class=\"calc\",le=\"30\"} 2\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_request_duration_seconds_bucket{class=\"calc\",le=\"+Inf\"} 3\n"));
    EXPECT_TRUE(contains(t, "rfmap_http_request_duration_seconds_count{class=\"default\"} 1\n"));
}

TEST(HttpMetricsTest, RequestThreadPool) {
    HttpMetrics m;
    m.enqueued();
    m.enqueued();
    m.started();
    PoolStats s = m.pool(16);
    EXPECT_EQ(s.threads, 16);
    EXPECT_EQ(s.queued, 1u);
    EXPECT_EQ(s.busy, 1);
    m.finished();
    s = m.pool(16);
    EXPECT_EQ(s.busy, 0);
    EXPECT_EQ(s.completed, 1u);
}

TEST(ThreadPoolTest, ReportsQueuedBusyAndCompleted) {
    ThreadPool pool(1);
    std::latch running(1);
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    auto first = pool.submit([&running, gate] {
        running.count_down();
        gate.wait();
    });
    auto second = pool.submit([] {});
    running.wait();
    PoolStats s = pool.stats();
    EXPECT_EQ(s.threads, 1);
    EXPECT_EQ(s.busy, 1);
    EXPECT_EQ(s.queued, 1u);
    release.set_value();
    first.get();
    second.get();
    // The counters move just after the task's future is ready.
    for (int i = 0; i < 1000 && pool.stats().completed < 2; ++i) std::this_thread::yield();
    s = pool.stats();
    EXPECT_EQ(s.completed, 2u);
    EXPECT_EQ(s.queued, 0u);
}

TEST(ServerMetricsTest, WritesEveryFamily) {
    ServerSample sample;
    sample.version = "1.2.3";
    sample.startTimeSeconds = 1700000000;
    sample.pools = {{"http", {16, 2, 3, 40}}, {"job-cells", {4, 0, 0, 7}}};
    sample.db = {8, 5, 1, 120, 0.25, 2};
    sample.jobs.byState = {{"running", 1}, {"done", 3}};
    sample.jobs.started = 4;
    CacheStats dem;
    dem.limitBytes = 100;
    dem.usedBytes = 60;
    dem.entries = 3;
    dem.hits = 9;
    dem.misses = 1;
    dem.evictions = 2;
    CacheStats table;
    table.hits = 5;
    table.misses = 6;
    sample.caches = {{"dem-blocks", dem}, {"results-table", table, false}};
    PrometheusText out;
    writeServerMetrics(out, sample);
    const std::string& t = out.text();
    for (const char* line : {"rfmap_build_info{version=\"1.2.3\"} 1\n", "rfmap_process_start_time_seconds 1.7e+09\n",
                             "rfmap_pool_queued_tasks{pool=\"http\"} 2\n", "rfmap_pool_busy_threads{pool=\"http\"} 3\n",
                             "rfmap_pool_tasks_completed_total{pool=\"job-cells\"} 7\n", "rfmap_db_pool_waiting 1\n",
                             "rfmap_db_pool_wait_seconds_total 0.25\n", "rfmap_db_pool_reconnects_total 2\n",
                             "rfmap_jobs{state=\"done\"} 3\n", "rfmap_jobs_started_total 4\n",
                             "rfmap_cache_hits_total{tier=\"results-table\"} 5\n",
                             "rfmap_cache_used_bytes{tier=\"dem-blocks\"} 60\n"}) {
        EXPECT_TRUE(contains(t, line)) << line;
    }
    // The table tier has no size without a query, so it has no size samples.
    EXPECT_FALSE(contains(t, "rfmap_cache_used_bytes{tier=\"results-table\"}"));
}
