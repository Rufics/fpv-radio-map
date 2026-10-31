#include "database.hpp"

namespace rfmap {

namespace {

[[noreturn]] void unavailable(const char* why) {
    throw HttpError(503, std::string("the database is ") + why + "; try again in a few seconds", {{"retryAfterS", 2}});
}

}  // namespace

Database::Database(const std::string& url, const Settings& settings) {
    try {
        pool_ = std::make_unique<rfdb::pg::SessionPool>(url, sessionOptions(settings), poolSettings(settings));
    } catch (const rfdb::ConnectionFailed& e) {
        log_.error("database", e.what());
        unavailable("unavailable");
    }
}

rfdb::pg::SessionOptions Database::sessionOptions(const Settings& s) {
    return {.statementTimeout = s.statementTimeout,
            .lockTimeout = s.lockTimeout,
            .idleInTransactionTimeout = s.idleInTransactionTimeout,
            .connectTimeout = s.connectTimeout,
            .applicationName = "rfmap-server"};
}

rfdb::PoolSettings Database::poolSettings(const Settings& s) {
    rfdb::PoolSettings p;
    p.maxSize = s.poolSize;
    p.minIdle = s.minIdle;
    p.leaseTimeout = s.leaseTimeout;
    p.idleTimeout = s.idleTimeout;
    p.maxLifetime = s.maxLifetime;
    return p;
}

// A function-try-block, so the pool's own errors from the member's construction are translated.
Database::Lease::Lease(Database& db) try : lease_(*db.pool_) {
} catch (const rfdb::PoolExhausted&) {
    unavailable("busy");
} catch (const rfdb::ConnectionFailed& e) {
    db.log_.warn("database", e.what());
    unavailable("unavailable");
}

DbPoolStats Database::stats() const {
    const rfdb::PoolStats s = pool_->stats();
    return {.size = s.open,
            .maxSize = s.maxSize,
            .idle = s.idle,
            .waiting = s.waiting,
            .leases = s.leases,
            .waitSeconds = s.waitSeconds,
            .reconnects = s.broken,
            .leaseTimeouts = s.leaseTimeouts,
            .opened = s.opened,
            .closed = s.closed,
            .connectFailures = s.connectFailures};
}

}  // namespace rfmap
