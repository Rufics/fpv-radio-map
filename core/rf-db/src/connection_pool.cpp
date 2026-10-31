#include "rfdb/connection_pool.hpp"

#include <algorithm>
#include <utility>

namespace rfdb {

namespace {

PoolSettings sane(PoolSettings s) {
    s.maxSize = std::max(1, s.maxSize);
    s.minIdle = std::clamp(s.minIdle, 0, s.maxSize);
    return s;
}

}  // namespace

ConnectionPool::Lease::Lease(ConnectionPool& pool, std::unique_ptr<Connection> conn, Clock::time_point created)
    : pool_(pool), conn_(std::move(conn)), created_(created) {}

ConnectionPool::Lease::~Lease() { pool_.giveBack(std::move(conn_), created_, fate_); }

ConnectionPool::ConnectionPool(std::unique_ptr<ConnectionFactory> factory, PoolSettings settings)
    : factory_(std::move(factory)), settings_(sane(settings)) {
    for (int i = 0; i < settings_.minIdle; ++i) {
        {
            const std::lock_guard lock(mutex_);
            ++open_;
        }
        auto conn = openReserved();  // throws ConnectionFailed: the pool is not built
        giveBack(std::move(conn), Clock::now(), Lease::Fate::Return);
    }
    if (settings_.maintenanceInterval.count() > 0) {
        housekeeper_ = std::jthread([this](const std::stop_token& stop) { housekeep(stop); });
    }
}

ConnectionPool::~ConnectionPool() {
    housekeeper_.request_stop();
    wake_.notify_all();
}

ConnectionPool::Lease ConnectionPool::acquire() {
    const Clock::time_point deadline = Clock::now() + settings_.leaseTimeout;
    for (;;) {
        std::optional<Idle> idle = waitForGrant(deadline);
        if (!idle) {
            // Room reserved: open a connection of our own, outside the lock.
            return {*this, openReserved(), Clock::now()};
        }
        if (usable(*idle, Clock::now())) return {*this, std::move(idle->conn), idle->created};
        const bool broken = !idle->conn->isOpen();
        closeUnusable(std::move(idle->conn), broken);
    }
}

std::optional<ConnectionPool::Idle> ConnectionPool::waitForGrant(Clock::time_point deadline) {
    const Clock::time_point asked = Clock::now();
    std::unique_lock lock(mutex_);
    ++waiting_;
    const bool granted = available_.wait_until(lock, deadline, [this] { return grantableLocked(); });
    --waiting_;
    counters_.waitSeconds += std::chrono::duration<double>(Clock::now() - asked).count();
    if (!granted) {
        ++counters_.leaseTimeouts;
        throw PoolExhausted("no database connection became free within the lease timeout");
    }
    ++counters_.leases;
    if (idle_.empty()) {
        ++open_;
        return std::nullopt;
    }
    Idle idle = std::move(idle_.back());
    idle_.pop_back();
    return idle;
}

bool ConnectionPool::usable(Idle& idle, Clock::time_point now) const {
    if (!idle.conn->isOpen() || expired(idle.created, now)) return false;
    if (now - idle.since < settings_.validateAfterIdle) return true;
    return idle.conn->ping();
}

bool ConnectionPool::expired(Clock::time_point created, Clock::time_point now) const {
    return settings_.maxLifetime.count() > 0 && now - created >= settings_.maxLifetime;
}

std::unique_ptr<Connection> ConnectionPool::openReserved() {
    try {
        auto conn = factory_->open();
        const std::lock_guard lock(mutex_);
        ++counters_.opened;
        return conn;
    } catch (const ConnectionFailed&) {
        {
            const std::lock_guard lock(mutex_);
            --open_;
            ++counters_.connectFailures;
        }
        // The room is free again: a waiter may try a connect of its own.
        available_.notify_one();
        throw;
    }
}

void ConnectionPool::closeUnusable(std::unique_ptr<Connection> conn, bool broken) {
    conn.reset();  // closes the session, outside the lock
    {
        const std::lock_guard lock(mutex_);
        --open_;
        ++counters_.closed;
        if (broken) ++counters_.broken;
    }
    available_.notify_one();
}

void ConnectionPool::giveBack(std::unique_ptr<Connection> conn, Clock::time_point created, Lease::Fate fate) {
    const Clock::time_point now = Clock::now();
    const bool broken = fate == Lease::Fate::Broken || !conn->isOpen();
    if (broken || fate == Lease::Fate::Retire || expired(created, now)) {
        closeUnusable(std::move(conn), broken);
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        idle_.push_back({std::move(conn), created, now});
    }
    available_.notify_one();
}

std::vector<std::unique_ptr<Connection>> ConnectionPool::evictLocked(Clock::time_point now) {
    std::vector<std::unique_ptr<Connection>> out;
    const bool idleLimit = settings_.idleTimeout.count() > 0;
    // Oldest returns first: those are the ones the busy end of the deque no longer reaches.
    for (auto it = idle_.begin(); it != idle_.end();) {
        const bool tooOld = expired(it->created, now);
        const bool rested = idleLimit && now - it->since >= settings_.idleTimeout && open_ > settings_.minIdle;
        if (tooOld || rested) {
            out.push_back(std::move(it->conn));
            it = idle_.erase(it);
            --open_;
            ++counters_.closed;
        } else {
            ++it;
        }
    }
    return out;
}

void ConnectionPool::maintain() {
    int missing = 0;
    {
        std::vector<std::unique_ptr<Connection>> closing;
        {
            const std::lock_guard lock(mutex_);
            closing = evictLocked(Clock::now());
            missing = std::max(0, settings_.minIdle - open_);
            open_ += missing;
        }
        // `closing` goes out of scope here: the sessions close without the lock held.
    }
    available_.notify_all();
    for (; missing > 0; --missing) {
        try {
            giveBack(openReserved(), Clock::now(), Lease::Fate::Return);
        } catch (const ConnectionFailed&) {
            // Counted in openReserved; give back the rest of the room and try again next round.
            const std::lock_guard lock(mutex_);
            open_ -= missing - 1;
            break;
        }
    }
}

void ConnectionPool::housekeep(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    const auto stopped = [&stop] { return stop.stop_requested(); };
    while (!wake_.wait_for(lock, stop, settings_.maintenanceInterval, stopped)) {
        lock.unlock();
        maintain();
        lock.lock();
    }
}

PoolStats ConnectionPool::stats() const {
    const std::lock_guard lock(mutex_);
    PoolStats s = counters_;
    s.maxSize = settings_.maxSize;
    s.open = open_;
    s.idle = static_cast<int>(idle_.size());
    s.waiting = waiting_;
    return s;
}

}  // namespace rfdb
