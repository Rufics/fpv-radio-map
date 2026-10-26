#pragma once

// What GET /readyz on the operations listener reports: a set of named checks, each answering
// whether the server can take traffic now. A check that fails makes the whole answer 503, so a
// load balancer or Kubernetes stops sending requests to this instance until it passes again.

#include "common.hpp"
#include "database.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rfmap {

/// One check's answer.
struct CheckResult {
    bool ok = true;
    std::string detail;  ///< why it failed, or a short status when it passed
};

/// The checks behind /readyz, run in order on every request. Readiness is the conjunction of the
/// required checks; informational ones (a missing optional dataset, for example) are reported
/// but do not fail the answer.
class ReadinessChecks {
public:
    using Check = std::function<CheckResult()>;

    void addRequired(std::string name, Check check) { checks_.emplace_back(std::move(name), std::move(check), true); }
    void addInformational(std::string name, Check check) {
        checks_.emplace_back(std::move(name), std::move(check), false);
    }

    struct Report {
        bool ready = true;
        json body;  ///< {"ready": bool, "checks": {name: {"ok": bool, "detail": ...}}}
    };
    [[nodiscard]] Report run() const;

private:
    struct Entry {
        std::string name;
        Check check;
        bool required;
    };
    std::vector<Entry> checks_;
};

/// Whether PostgreSQL answers, on a connection of its own so that a probe never waits behind the
/// request threads for a pooled connection. One probe runs at a time; a probe that arrives while
/// another is in flight gets the last answer, so a slow database cannot pile up connections, and
/// a failure once the probe in flight has outlived its budget, so a black-holed database cannot
/// keep an old "ready" alive.
class DatabaseProbe {
public:
    /// `statementTimeout` bounds the probe query on the server side; the client side gets a
    /// connect timeout and a TCP user timeout of the same order (see probeUrl).
    DatabaseProbe(const std::string& url, std::chrono::milliseconds statementTimeout, Logger& log);
    ~DatabaseProbe();
    DatabaseProbe(const DatabaseProbe&) = delete;
    DatabaseProbe& operator=(const DatabaseProbe&) = delete;
    DatabaseProbe(DatabaseProbe&&) = delete;
    DatabaseProbe& operator=(DatabaseProbe&&) = delete;

    [[nodiscard]] CheckResult check();

    /// `url` with client-side bounds added unless it names them already: connect_timeout (whole
    /// seconds, at least 2, as libpq rounds anyway) and tcp_user_timeout (milliseconds), so a
    /// probe is bounded even when the network drops packets instead of refusing. Takes a URI
    /// ("postgresql://...") or a keyword string ("host=... dbname=...").
    static std::string probeUrl(const std::string& url, std::chrono::milliseconds statementTimeout);
    /// The longest a well-behaved probe takes: connect timeout plus statement timeout.
    [[nodiscard]] std::chrono::milliseconds budget() const { return budget_; }

private:
    /// Connects when there is no open connection; throws pqxx::failure when it cannot.
    void ensureConnected();
    CheckResult probe();

    using Clock = std::chrono::steady_clock;

    std::string url_;
    std::chrono::milliseconds statementTimeout_;
    std::chrono::milliseconds budget_;
    Logger& log_;
    std::mutex mutex_;  ///< serialises probes and guards conn_
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex lastMutex_;  ///< guards last_ and started_
    CheckResult last_{false, "not probed yet"};
    Clock::time_point started_{};  ///< when the probe in flight began; meaningful while mutex_ is held
};

}  // namespace rfmap
