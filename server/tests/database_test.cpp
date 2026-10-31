// The connection pool over a real database: a caller that finds every connection lent gets 503
// after the lease timeout, sessions open on demand up to the pool size, a broken session is
// replaced, and every pooled session carries the configured statement and lock timeouts.
// Linked into the integration binary, which ctest gives a throw-away database.

#include "database.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

using namespace rfmap;
using namespace std::chrono_literals;

namespace {

class DatabasePoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* url = std::getenv("RFMAP_TEST_DATABASE_URL");
        if (url == nullptr) GTEST_SKIP() << "RFMAP_TEST_DATABASE_URL not set (run through ctest)";
        url_ = url;
    }

    static std::string show(Database& db, const std::string& setting) {
        return db.with([&setting](pqxx::connection& c) {
            return pqxx::nontransaction(c).query_value<std::string>("show " + setting);
        });
    }
    static std::string statementTimeout(Database& db) { return show(db, "statement_timeout"); }

    std::string url_;
};

}  // namespace

TEST_F(DatabasePoolTest, LeaseTimesOutWith503WhileEveryConnectionIsLent) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 50ms, .statementTimeout = 0ms});
    db.with([&db](pqxx::connection&) {
        const auto started = std::chrono::steady_clock::now();
        try {
            db.with([](pqxx::connection&) { return 0; });
            ADD_FAILURE() << "expected 503";
        } catch (const HttpError& e) {
            EXPECT_EQ(e.status(), 503);
            EXPECT_EQ(e.body().at("retryAfterS").get<int>(), 2);
        }
        EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);
        return 0;
    });
    const DbPoolStats stats = db.stats();
    EXPECT_EQ(stats.leaseTimeouts, 1u);
    EXPECT_EQ(stats.leases, 1u);
    EXPECT_EQ(stats.idle, 1);
    EXPECT_EQ(stats.size, 1);
    EXPECT_EQ(stats.waiting, 0);
    // The pool still lends once the connection is back.
    EXPECT_EQ(db.with([](pqxx::connection&) { return 7; }), 7);
}

TEST_F(DatabasePoolTest, SessionsCarryTheStatementTimeout) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 1s, .statementTimeout = 1500ms});
    EXPECT_EQ(statementTimeout(db), "1500ms");
    EXPECT_THROW(db.with([](pqxx::connection& c) { return pqxx::nontransaction(c).exec("select pg_sleep(3)"); }),
                 pqxx::sql_error);
    // A cancelled statement leaves a usable session behind.
    EXPECT_EQ(statementTimeout(db), "1500ms");
}

TEST_F(DatabasePoolTest, LongWorkRunsWithoutTimeoutsAndLeavesNoTrace) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 1s, .statementTimeout = 1500ms, .lockTimeout = 700ms});
    const auto lifted = db.withoutTimeouts([](pqxx::connection& c) {
        pqxx::nontransaction n(c);
        return std::pair(n.query_value<std::string>("show statement_timeout"), n.query_value<std::string>("show lock_timeout"));
    });
    EXPECT_EQ(lifted, std::pair(std::string("0"), std::string("0")));
    // The session was closed and a fresh one opened, not counted as a broken one.
    EXPECT_EQ(statementTimeout(db), "1500ms");
    EXPECT_EQ(show(db, "lock_timeout"), "700ms");
    EXPECT_EQ(db.stats().reconnects, 0u);
    EXPECT_EQ(db.stats().opened, 2u);
}

TEST_F(DatabasePoolTest, OpensSessionsOnDemandUpToThePoolSize) {
    Database db(url_, {.poolSize = 3, .leaseTimeout = 50ms, .minIdle = 0});
    EXPECT_EQ(db.stats().size, 0);
    db.with([&db](pqxx::connection&) {
        db.with([&db](pqxx::connection&) {
            EXPECT_EQ(db.stats().size, 2);
            return 0;
        });
        return 0;
    });
    // Both are reused afterwards: no third session for a third sequential caller.
    for (int i = 0; i < 3; ++i) db.with([](pqxx::connection&) { return 0; });
    EXPECT_EQ(db.stats().opened, 2u);
    EXPECT_EQ(db.stats().maxSize, 3);
}

TEST_F(DatabasePoolTest, ABrokenSessionIsReplacedForTheNextCaller) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 1s});
    EXPECT_THROW(db.with([](pqxx::connection& c) {
        // The server ends this very session, as a restart or an admin would.
        return pqxx::nontransaction(c).exec("select pg_terminate_backend(pg_backend_pid())");
    }),
                 pqxx::failure);
    EXPECT_EQ(db.with([](pqxx::connection& c) { return pqxx::nontransaction(c).query_value<int>("select 7"); }), 7);
    EXPECT_EQ(db.stats().reconnects, 1u);
}

TEST_F(DatabasePoolTest, ALockHeldByOneSessionTimesOutTheOthersQuickly) {
    Database db(url_, {.poolSize = 2, .leaseTimeout = 1s, .lockTimeout = 200ms});
    db.with([](pqxx::connection& c) {
        return pqxx::nontransaction(c).exec("create table if not exists pool_lock_probe (id int)");
    });
    db.tx([&db](pqxx::work& holder) {
        holder.exec("lock table pool_lock_probe in access exclusive mode");
        const auto started = std::chrono::steady_clock::now();
        EXPECT_THROW(db.tx([](pqxx::work& w) { return w.exec("select count(*) from pool_lock_probe"); }), pqxx::sql_error);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    });
    db.with([](pqxx::connection& c) { return pqxx::nontransaction(c).exec("drop table pool_lock_probe"); });
}

TEST_F(DatabasePoolTest, SessionsCarryTheLockAndIdleTransactionTimeouts) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 1s, .lockTimeout = 700ms, .idleInTransactionTimeout = 1200ms});
    EXPECT_EQ(show(db, "lock_timeout"), "700ms");
    EXPECT_EQ(show(db, "idle_in_transaction_session_timeout"), "1200ms");
    EXPECT_EQ(show(db, "application_name"), "rfmap-server");
}

TEST_F(DatabasePoolTest, AnUnreachableDatabaseAnswers503AtStart) {
    try {
        Database db("postgresql://rfmap@127.0.0.1:1/none", {.poolSize = 1, .connectTimeout = 2s});
        ADD_FAILURE() << "expected 503";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.status(), 503);
    }
}
