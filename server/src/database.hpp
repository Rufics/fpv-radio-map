#pragma once

// The distribution's libpqxx 7.8 is built as C++17, so its exception constructors take no
// std::source_location. Compiled as C++20, its headers would declare ones that take it and
// the link fails; hide the feature so the headers match the library's ABI.
#include <version>
#undef __cpp_lib_source_location  // NOSONAR(cpp:S959): a library feature macro <version> defines; see above
#include <pqxx/pqxx>

#include "common.hpp"
#include "logger.hpp"
#include "metrics.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rfmap {

/// A fixed pool of PostgreSQL connections. `with` lends one for the duration of a callback and
/// replaces it if it broke, so one dropped connection does not poison the pool. A caller that
/// finds every connection lent waits at most `leaseTimeout` and then gets 503, and every session
/// carries a statement_timeout, so a slow database turns into quick errors, not hung threads.
class Database {
public:
    struct Settings {
        int poolSize = 8;
        std::chrono::milliseconds leaseTimeout{10000};
        std::chrono::milliseconds statementTimeout{60000};  ///< 0 = the server's own setting
    };

    Database(std::string url, Settings settings);

    /// Throws HttpError 503 {retryAfterS} when no connection frees up within the lease timeout.
    template <class F>
    auto with(F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        Lease lease(*this);
        return lend(lease, std::forward<F>(f));
    }

    /// `with` on a session without a statement_timeout, for work that is long by nature (schema
    /// migrations at start). The session is replaced afterwards, so the lifted limit leaves with it.
    template <class F>
    auto withoutStatementTimeout(F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        Lease lease(*this);
        lease.retire = true;
        pqxx::nontransaction(*lease.conn).exec("SET statement_timeout = 0");
        return lend(lease, std::forward<F>(f));
    }

    /// Runs `f` in a transaction and commits it.
    template <class F>
    auto tx(F&& f) -> decltype(f(std::declval<pqxx::work&>())) {
        return with([&f](pqxx::connection& c) {
            pqxx::work w(c);
            if constexpr (std::is_void_v<decltype(f(w))>) {
                std::forward<F>(f)(w);
                w.commit();
            } else {
                auto r = std::forward<F>(f)(w);
                w.commit();
                return r;
            }
        });
    }

    [[nodiscard]] DbPoolStats stats();

private:
    /// Returns a leased connection to the pool and wakes one waiting lease.
    void giveBack(std::unique_ptr<pqxx::connection> c);

    struct Lease {
        explicit Lease(Database& db);
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Database& db;
        std::unique_ptr<pqxx::connection> conn;
        bool broken = false;  ///< the connection dropped: replace it and count a reconnect
        bool retire = false;  ///< the session was changed for one caller: replace it
        /// Puts the pool's settings back on a retired session that could not be replaced.
        void restore();
    };

    template <class F>
    static auto lend(Lease& lease, F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        try {
            return std::forward<F>(f)(*lease.conn);
        } catch (const pqxx::broken_connection&) {
            lease.broken = true;
            throw;
        }
    }

    /// A new session with the pool's settings applied.
    [[nodiscard]] std::unique_ptr<pqxx::connection> connect() const;
    void applySettings(pqxx::connection& c) const;
    [[noreturn]] void busy();

    std::string url_;
    Settings settings_;
    std::mutex mutex_;
    // Guarded by mutex_, except reconnects_, which the lease updates outside the lock.
    int waiting_ = 0;
    std::uint64_t leases_ = 0;
    std::uint64_t leaseTimeouts_ = 0;
    double waitSeconds_ = 0.0;
    std::atomic<std::uint64_t> reconnects_{0};
    std::condition_variable available_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
    Logger log_;
};

}  // namespace rfmap
