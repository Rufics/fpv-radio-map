#pragma once

#include "rfdb/connection.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace rfdb {

using Clock = std::chrono::steady_clock;

/// How a ConnectionPool sizes itself and when it lets a connection go.
struct PoolSettings {
    /// The most connections the pool keeps open at once, lent or idle. The database server sees
    /// at most this many sessions from one pool, however many callers ask.
    int maxSize = 8;
    /// Connections kept open while the pool is quiet, so the first request after a pause does
    /// not pay for a connect. Capped by maxSize.
    int minIdle = 1;
    /// How long a caller waits for a connection when all maxSize are lent; then PoolExhausted.
    std::chrono::milliseconds leaseTimeout{10000};
    /// A connection idle this long is closed, down to minIdle. 0 keeps idle connections open.
    std::chrono::milliseconds idleTimeout{300000};
    /// A connection this old is closed instead of lent again, so sessions do not live forever
    /// (server-side memory, a changed DNS name or role settings). 0 = no limit.
    std::chrono::milliseconds maxLifetime{1800000};
    /// A connection idle this long is pinged before it is lent, so a session the server or a
    /// firewall dropped is replaced instead of failing the caller's first query. 0 pings every time.
    std::chrono::milliseconds validateAfterIdle{30000};
    /// How often the housekeeper closes idle and old connections and tops up to minIdle.
    /// 0 = no housekeeper thread (the owner calls maintain() itself; tests do).
    std::chrono::milliseconds maintenanceInterval{15000};
};

/// What the pool has done since it started, for metrics.
struct PoolStats {
    int maxSize = 0;
    int open = 0;     ///< connections open now, lent or idle (and those being opened)
    int idle = 0;     ///< connections waiting to be lent
    int waiting = 0;  ///< callers waiting for a connection
    std::uint64_t leases = 0;           ///< connections lent
    double waitSeconds = 0.0;           ///< total time callers waited for one
    std::uint64_t leaseTimeouts = 0;    ///< callers turned away with PoolExhausted
    std::uint64_t opened = 0;           ///< connections opened
    std::uint64_t closed = 0;           ///< connections closed: idle, too old, retired or broken
    std::uint64_t broken = 0;           ///< of those, closed because they had stopped working
    std::uint64_t connectFailures = 0;  ///< factory calls that failed
};

/// A bounded pool of database connections that opens them on demand and reuses them.
///
/// - At most maxSize connections are open; a caller that finds them all lent waits at most
///   leaseTimeout and then gets PoolExhausted, so a stuck database turns into quick errors
///   instead of piling up threads or sessions.
/// - The lock guards only bookkeeping: connects, pings and closes run outside it, so one slow
///   connection never holds up callers that could take or return another one.
/// - A connection that broke, or that a caller changed for its own use (retire), is closed when
///   it comes back, and the next caller that needs one opens a fresh one.
/// - Idle connections above minIdle and connections past maxLifetime are closed by the
///   housekeeper; connections idle longer than validateAfterIdle are pinged before they are lent.
///
/// The pool must outlive every Lease it handed out.
class ConnectionPool {
public:
    /// A connection lent to one caller; it goes back to the pool when the lease ends, whatever
    /// way the caller leaves.
    class Lease {
    public:
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = delete;
        Lease& operator=(Lease&&) = delete;

        [[nodiscard]] Connection& connection() const { return *conn_; }
        /// The connection stopped working: close it and count it as broken.
        void markBroken() { fate_ = Fate::Broken; }
        /// The caller changed the session for itself: close it instead of lending it again.
        void retire() {
            if (fate_ == Fate::Return) fate_ = Fate::Retire;
        }

    private:
        friend class ConnectionPool;
        enum class Fate { Return, Retire, Broken };
        Lease(ConnectionPool& pool, std::unique_ptr<Connection> conn, Clock::time_point created);

        ConnectionPool& pool_;
        std::unique_ptr<Connection> conn_;
        Clock::time_point created_;
        Fate fate_ = Fate::Return;
    };

    /// Opens minIdle connections at once, so a database that is not there fails the start-up
    /// (ConnectionFailed) instead of the first request.
    ConnectionPool(std::unique_ptr<ConnectionFactory> factory, PoolSettings settings);
    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    /// Throws PoolExhausted when no connection frees up within the lease timeout, and
    /// ConnectionFailed when the pool had room for a new connection but could not open one.
    [[nodiscard]] Lease acquire();

    /// Closes idle connections above minIdle that have been idle for idleTimeout and idle ones
    /// past maxLifetime, then opens connections up to minIdle. Never throws: a failed connect is
    /// counted and tried again next time.
    void maintain();

    [[nodiscard]] PoolStats stats() const;
    [[nodiscard]] const PoolSettings& settings() const { return settings_; }

private:
    struct Idle {
        std::unique_ptr<Connection> conn;
        Clock::time_point created;
        Clock::time_point since;  ///< when it came back
    };
    /// Waits until a connection is idle or there is room for another, and takes it: the idle
    /// connection, or nullopt with the room reserved in open_. Throws PoolExhausted at the deadline.
    [[nodiscard]] std::optional<Idle> waitForGrant(Clock::time_point deadline);
    [[nodiscard]] bool grantableLocked() const { return !idle_.empty() || open_ < settings_.maxSize; }
    /// An idle connection is lent only if it is open, young enough and, after a long rest, answers a ping.
    [[nodiscard]] bool usable(Idle& idle, Clock::time_point now) const;
    [[nodiscard]] bool expired(Clock::time_point created, Clock::time_point now) const;
    /// Opens a connection for room already reserved in open_; gives the room back if it fails.
    [[nodiscard]] std::unique_ptr<Connection> openReserved();
    void giveBack(std::unique_ptr<Connection> conn, Clock::time_point created, Lease::Fate fate);
    /// Takes the idle connections maintain() closes out of idle_; the caller closes them unlocked.
    [[nodiscard]] std::vector<std::unique_ptr<Connection>> evictLocked(Clock::time_point now);
    void closeUnusable(std::unique_ptr<Connection> conn, bool broken);
    void housekeep(const std::stop_token& stop);

    std::unique_ptr<ConnectionFactory> factory_;
    PoolSettings settings_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    // Guarded by mutex_.
    std::deque<Idle> idle_;  ///< most recently returned at the back: lent first, so the front ages out
    int open_ = 0;           ///< lent + idle + being opened; never above maxSize
    int waiting_ = 0;
    PoolStats counters_;
    // Last: stops and joins before the members it uses go away.
    std::condition_variable_any wake_;
    std::jthread housekeeper_;
};

}  // namespace rfdb
