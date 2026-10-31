#include "readiness.hpp"

#include <rfdb/pg/connection_string.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <string_view>

namespace rfmap {

ReadinessChecks::Report ReadinessChecks::run() const {
    Report report;
    report.body = {{"ready", true}, {"checks", json::object()}};
    for (const Entry& e : checks_) {
        CheckResult r;
        try {
            r = e.check();
        } catch (const std::exception& ex) {  // NOSONAR(cpp:S1181): a probe that throws is a failed probe
            r = {false, ex.what()};
        }
        json entry = {{"ok", r.ok}, {"required", e.required}};
        if (!r.detail.empty()) entry["detail"] = r.detail;
        report.body["checks"][e.name] = std::move(entry);
        if (e.required && !r.ok) report.ready = false;
    }
    report.body["ready"] = report.ready;
    return report;
}

namespace {

constexpr std::chrono::seconds kMinConnectTimeout{2};  // libpq treats anything shorter as 2 s

std::chrono::seconds connectTimeoutFor(std::chrono::milliseconds statementTimeout) {
    return std::max(std::chrono::ceil<std::chrono::seconds>(statementTimeout), kMinConnectTimeout);
}

}  // namespace

std::string DatabaseProbe::probeUrl(const std::string& url, std::chrono::milliseconds statementTimeout) {
    rfdb::pg::ConnectionString s(url);
    s.withDefault("connect_timeout", connectTimeoutFor(statementTimeout).count())
        .withDefault("tcp_user_timeout", statementTimeout.count());
    return s.text();
}

DatabaseProbe::DatabaseProbe(const std::string& url, std::chrono::milliseconds statementTimeout, Logger& log)
    : url_(probeUrl(url, statementTimeout)), statementTimeout_(statementTimeout),
      budget_(connectTimeoutFor(statementTimeout) + statementTimeout), log_(log) {}

DatabaseProbe::~DatabaseProbe() = default;

CheckResult DatabaseProbe::check() {
    std::unique_lock lock(mutex_, std::defer_lock);
    {
        // Winning the probe and stamping its start happen under lastMutex_ together, so a caller
        // that loses never reads the start of an earlier probe as this one's.
        const std::lock_guard last(lastMutex_);
        if (!lock.try_lock()) {
            // Another probe is in flight. Its last answer stands while it is within budget; past
            // that the database is not answering in time, whatever it said before.
            if (Clock::now() - started_ > budget_) return {false, "database probe timed out"};
            return last_;
        }
        started_ = Clock::now();
    }
    CheckResult result = probe();
    const std::lock_guard last(lastMutex_);
    last_ = result;
    return result;
}

void DatabaseProbe::ensureConnected() {
    if (conn_ && conn_->is_open()) return;
    conn_ = std::make_unique<pqxx::connection>(url_);
    // Session-level, so it holds for every later probe on this connection.
    pqxx::nontransaction tx(*conn_);
    tx.exec(std::format("set statement_timeout = {}", statementTimeout_.count()));
}

CheckResult DatabaseProbe::probe() {
    try {
        ensureConnected();
        pqxx::nontransaction tx(*conn_);
        (void)tx.exec("select 1").one_field();
        return {true, "ok"};
    } catch (const pqxx::failure& e) {
        // Drop the connection: the next probe reconnects instead of reusing a broken one.
        conn_.reset();
        log_.warn("readiness", std::string("database probe failed: ") + e.what());
        return {false, "database unreachable"};
    }
}

}  // namespace rfmap
