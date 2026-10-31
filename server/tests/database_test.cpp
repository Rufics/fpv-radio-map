// The connection pool over a real database: a caller that finds every connection lent gets 503
// after the lease timeout, and every pooled session carries the configured statement_timeout.
// Linked into the integration binary, which ctest gives a throw-away database.

#include "database.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>

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

    static std::string statementTimeout(Database& db) {
        return db.with([](pqxx::connection& c) {
            return pqxx::nontransaction(c).query_value<std::string>("show statement_timeout");
        });
    }

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

TEST_F(DatabasePoolTest, LongWorkRunsWithoutTheTimeoutAndLeavesNoTrace) {
    Database db(url_, {.poolSize = 1, .leaseTimeout = 1s, .statementTimeout = 1500ms});
    const std::string lifted = db.withoutStatementTimeout([](pqxx::connection& c) {
        return pqxx::nontransaction(c).query_value<std::string>("show statement_timeout");
    });
    EXPECT_EQ(lifted, "0");
    // The session was replaced, not counted as a broken one.
    EXPECT_EQ(statementTimeout(db), "1500ms");
    EXPECT_EQ(db.stats().reconnects, 0u);
}
