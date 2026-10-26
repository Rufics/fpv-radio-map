#include "readiness.hpp"

#include <exception>

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

DatabaseProbe::DatabaseProbe(std::string url, std::chrono::milliseconds statementTimeout, Logger& log)
    : url_(std::move(url)), statementTimeout_(statementTimeout), log_(log) {}

DatabaseProbe::~DatabaseProbe() = default;

CheckResult DatabaseProbe::check() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        const std::lock_guard last(lastMutex_);
        return last_;
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
    tx.exec("set statement_timeout = " + std::to_string(statementTimeout_.count()));
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
