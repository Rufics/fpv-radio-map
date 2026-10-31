#pragma once

#include "rfdb/connection.hpp"
#include "rfdb/pg/pqxx.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace rfdb::pg {

/// What every pooled PostgreSQL session is set up with. A timeout of 0 leaves the role's or the
/// database's own setting.
struct SessionOptions {
    /// statement_timeout: PostgreSQL cancels a longer statement and the connection is free again.
    std::chrono::milliseconds statementTimeout{60000};
    /// lock_timeout: a statement waiting this long for a lock another session holds gives up,
    /// so one session that holds a lock cannot stall every other one behind it.
    std::chrono::milliseconds lockTimeout{10000};
    /// idle_in_transaction_session_timeout: PostgreSQL ends a session that sits in an open
    /// transaction this long, which releases the locks a stuck caller would otherwise keep.
    std::chrono::milliseconds idleInTransactionTimeout{60000};
    /// connect_timeout (whole seconds, at least 2 as libpq rounds), unless the URL sets one.
    std::chrono::milliseconds connectTimeout{5000};
    /// application_name, so pg_stat_activity shows whose sessions these are.
    std::string applicationName = "rfmap-server";
};

/// One PostgreSQL session for the pool.
class Session final : public Connection {
public:
    /// Throws pqxx::failure when it cannot connect.
    explicit Session(const std::string& connectionString);

    [[nodiscard]] bool isOpen() const override { return conn_.is_open(); }
    /// `select 1`; closes the session when it fails.
    [[nodiscard]] bool ping() override;

    /// Sets the session's timeouts; throws pqxx::failure.
    void apply(const SessionOptions& options);
    /// Turns every timeout off, for work that is long by nature (schema migrations). Lease the
    /// session with retire() so it is not lent again like that.
    void liftTimeouts();

    [[nodiscard]] pqxx::connection& raw() { return conn_; }

private:
    pqxx::connection conn_;
};

/// Opens sessions with client-side bounds (connect timeout, TCP keepalives and user timeout) so
/// a database that drops packets instead of refusing fails a connect in seconds, not minutes.
class SessionFactory final : public ConnectionFactory {
public:
    SessionFactory(const std::string& url, SessionOptions options);

    /// Throws ConnectionFailed.
    [[nodiscard]] std::unique_ptr<Connection> open() override;

    /// The connection string sessions use: `url` with the client-side defaults it does not set.
    [[nodiscard]] static std::string withClientBounds(const std::string& url, const SessionOptions& options);

private:
    std::string connectionString_;
    SessionOptions options_;
};

}  // namespace rfdb::pg
