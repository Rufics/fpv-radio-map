#pragma once

#include "common.hpp"
#include "logger.hpp"
#include "metrics.hpp"

#include <rfdb/pg/session_pool.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rfmap {

/// The server's door to PostgreSQL: every component that stores something goes through it, and
/// modules reach the data only through host calls that end up here.
///
/// A bounded rfdb::pg::SessionPool underneath: it opens sessions on demand up to `poolSize`,
/// keeps `minIdle` warm, closes the rest after `idleTimeout` and recycles sessions after
/// `maxLifetime`. `with` lends one for the duration of a callback; a session that broke is closed
/// and the next caller opens a fresh one, so one dropped connection does not poison the pool. A
/// caller that finds every connection lent waits at most `leaseTimeout` and then gets 503, and
/// every session carries a statement, lock and idle-in-transaction timeout, so a slow database or
/// a session holding locks turns into quick errors, not hung threads.
class Database {
public:
    struct Settings {
        int poolSize = 8;  ///< the most sessions open at once
        std::chrono::milliseconds leaseTimeout{10000};
        std::chrono::milliseconds statementTimeout{60000};  ///< 0 = the server's own setting
        std::chrono::milliseconds lockTimeout{10000};       ///< 0 = the server's own setting
        std::chrono::milliseconds idleInTransactionTimeout{60000};  ///< 0 = the server's own setting
        int minIdle = 1;
        std::chrono::milliseconds idleTimeout{300000};   ///< 0 = keep idle sessions open
        std::chrono::milliseconds maxLifetime{1800000};  ///< 0 = no limit
        std::chrono::milliseconds connectTimeout{5000};
    };

    /// Throws HttpError 503 when the first sessions cannot be opened.
    Database(const std::string& url, const Settings& settings);

    /// Throws HttpError 503 {retryAfterS} when no connection frees up within the lease timeout,
    /// or when a new session cannot be opened.
    template <class F>
    auto with(F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        Lease lease(*this);
        return lend(lease, std::forward<F>(f));
    }

    /// `with` on a session without any timeout, for work that is long by nature (schema
    /// migrations at start). The session is closed afterwards, so the lifted limits leave with it.
    template <class F>
    auto withoutTimeouts(F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        Lease lease(*this);
        lease.get().retire();
        lease.get().session().liftTimeouts();
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

    [[nodiscard]] DbPoolStats stats() const;

private:
    /// A pooled session, with the pool's own errors turned into the HTTP answer callers expect.
    class Lease {
    public:
        explicit Lease(Database& db);
        [[nodiscard]] rfdb::pg::SessionPool::Lease& get() { return *lease_; }

    private:
        std::optional<rfdb::pg::SessionPool::Lease> lease_;
    };

    template <class F>
    static auto lend(Lease& lease, F&& f) -> decltype(f(std::declval<pqxx::connection&>())) {
        try {
            return std::forward<F>(f)(lease.get().raw());
        } catch (const pqxx::broken_connection&) {
            lease.get().markBroken();
            throw;
        }
    }

    [[nodiscard]] static rfdb::pg::SessionOptions sessionOptions(const Settings& s);
    [[nodiscard]] static rfdb::PoolSettings poolSettings(const Settings& s);

    Logger log_;
    std::unique_ptr<rfdb::pg::SessionPool> pool_;
};

}  // namespace rfmap
