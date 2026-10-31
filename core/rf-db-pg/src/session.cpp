#include "rfdb/pg/session.hpp"

#include "rfdb/pg/connection_string.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace rfdb::pg {

namespace {

constexpr std::chrono::seconds kMinConnectTimeout{2};  // libpq treats anything shorter as 2 s
/// TCP keepalives on idle pooled sessions find a peer that vanished (failover, a dropped NAT
/// entry) within about a minute; the user timeout bounds unacknowledged writes the same way.
constexpr int kKeepalivesIdleS = 30;
constexpr int kKeepalivesIntervalS = 10;
constexpr int kKeepalivesCount = 3;
constexpr int kTcpUserTimeoutMs = 60000;

/// One set_config call per timeout that is set, in a single round trip.
class TimeoutSettings {
public:
    void add(std::string name, std::chrono::milliseconds value) {
        if (value.count() > 0) settings_.emplace_back(std::move(name), std::to_string(value.count()));
    }
    void exec(pqxx::connection& c) const {
        if (settings_.empty()) return;
        std::string sql = "select ";
        pqxx::params params;
        for (std::size_t i = 0; i < settings_.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += "set_config($" + std::to_string(2 * i + 1) + ", $" + std::to_string(2 * i + 2) + ", false)";
            params.append(settings_[i].first);
            params.append(settings_[i].second);
        }
        pqxx::nontransaction(c).exec(sql, params);
    }

private:
    std::vector<std::pair<std::string, std::string>> settings_;
};

}  // namespace

Session::Session(const std::string& connectionString) : conn_(connectionString) {}

bool Session::ping() {
    try {
        (void)pqxx::nontransaction(conn_).exec("select 1");
        return true;
    } catch (const pqxx::failure&) {
        conn_.close();
        return false;
    }
}

void Session::apply(const SessionOptions& options) {
    TimeoutSettings settings;
    settings.add("statement_timeout", options.statementTimeout);
    settings.add("lock_timeout", options.lockTimeout);
    settings.add("idle_in_transaction_session_timeout", options.idleInTransactionTimeout);
    settings.exec(conn_);
}

void Session::liftTimeouts() {
    pqxx::nontransaction(conn_).exec(
        "select set_config('statement_timeout', '0', false), set_config('lock_timeout', '0', false), "
        "set_config('idle_in_transaction_session_timeout', '0', false)");
}

SessionFactory::SessionFactory(const std::string& url, SessionOptions options)
    : connectionString_(withClientBounds(url, options)), options_(std::move(options)) {}

std::string SessionFactory::withClientBounds(const std::string& url, const SessionOptions& options) {
    const auto connect = std::max(std::chrono::ceil<std::chrono::seconds>(options.connectTimeout), kMinConnectTimeout);
    ConnectionString s(url);
    s.withDefault("connect_timeout", connect.count())
        .withDefault("keepalives", 1)
        .withDefault("keepalives_idle", kKeepalivesIdleS)
        .withDefault("keepalives_interval", kKeepalivesIntervalS)
        .withDefault("keepalives_count", kKeepalivesCount)
        .withDefault("tcp_user_timeout", kTcpUserTimeoutMs);
    if (!options.applicationName.empty()) s.withDefault("application_name", options.applicationName);
    return s.text();
}

std::unique_ptr<Connection> SessionFactory::open() {
    try {
        auto session = std::make_unique<Session>(connectionString_);
        session->apply(options_);
        return session;
    } catch (const pqxx::failure& e) {
        // libpq's message names the host and the reason, never the password.
        throw ConnectionFailed(std::string("cannot open a database session: ") + e.what());
    }
}

}  // namespace rfdb::pg
