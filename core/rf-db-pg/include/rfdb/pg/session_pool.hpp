#pragma once

#include "rfdb/connection_pool.hpp"
#include "rfdb/pg/session.hpp"

#include <string>

namespace rfdb::pg {

/// A ConnectionPool of PostgreSQL sessions, all set up with the same SessionOptions.
class SessionPool {
public:
    /// One session lent to one caller.
    class Lease {
    public:
        /// Throws PoolExhausted or ConnectionFailed.
        explicit Lease(SessionPool& pool) : lease_(pool.pool_.acquire()) {}

        [[nodiscard]] Session& session() const { return dynamic_cast<Session&>(lease_.connection()); }
        [[nodiscard]] pqxx::connection& raw() const { return session().raw(); }
        void markBroken() { lease_.markBroken(); }
        void retire() { lease_.retire(); }

    private:
        ConnectionPool::Lease lease_;
    };

    /// Throws ConnectionFailed when the first minIdle sessions cannot be opened.
    SessionPool(const std::string& url, SessionOptions options, PoolSettings settings);

    [[nodiscard]] PoolStats stats() const { return pool_.stats(); }

private:
    ConnectionPool pool_;
};

}  // namespace rfdb::pg
