#pragma once

#include <memory>
#include <stdexcept>
#include <string>

namespace rfdb {

/// Every error the pool raises: a caller that does not care why can catch this one.
class PoolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// No connection freed up, and none could be opened, within the lease timeout.
class PoolExhausted : public PoolError {
public:
    using PoolError::PoolError;
};

/// The factory could not open a connection: the database is unreachable or refused the login.
class ConnectionFailed : public PoolError {
public:
    using PoolError::PoolError;
};

/// One session with a database, as the pool sees it. The concrete class (rfdb::pg::Session for
/// PostgreSQL) adds the client API callers use; the pool only asks whether it is still usable.
class Connection {
public:
    virtual ~Connection() = default;
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    /// What the client library knows without talking to the server; must not block.
    [[nodiscard]] virtual bool isOpen() const = 0;
    /// One round trip to the server. False when the session is gone, and then isOpen() is false
    /// too; never throws.
    [[nodiscard]] virtual bool ping() = 0;
};

/// Opens connections for a pool. Called outside the pool's lock, so a slow connect delays only
/// the caller that needed a new connection. An implementation must bound how long it waits.
class ConnectionFactory {
public:
    virtual ~ConnectionFactory() = default;
    ConnectionFactory() = default;
    ConnectionFactory(const ConnectionFactory&) = delete;
    ConnectionFactory& operator=(const ConnectionFactory&) = delete;
    ConnectionFactory(ConnectionFactory&&) = delete;
    ConnectionFactory& operator=(ConnectionFactory&&) = delete;

    /// Throws ConnectionFailed.
    [[nodiscard]] virtual std::unique_ptr<Connection> open() = 0;
};

}  // namespace rfdb
