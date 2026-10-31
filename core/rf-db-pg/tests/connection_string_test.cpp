// Client-side defaults added to a libpq connection string; the operator's own settings win.

#include "rfdb/pg/connection_string.hpp"
#include "rfdb/pg/session.hpp"

#include <gtest/gtest.h>

using namespace rfdb::pg;
using namespace std::chrono_literals;

TEST(ConnectionString, AddsDefaultsToAUri) {
    ConnectionString s("postgresql://u@h/db");
    s.withDefault("connect_timeout", 5).withDefault("application_name", "rfmap");
    EXPECT_EQ(s.text(), "postgresql://u@h/db?connect_timeout=5&application_name=rfmap");
}

TEST(ConnectionString, AppendsToAnExistingQuery) {
    ConnectionString s("postgres://u@h/db?sslmode=require");
    s.withDefault("connect_timeout", 5);
    EXPECT_EQ(s.text(), "postgres://u@h/db?sslmode=require&connect_timeout=5");
}

TEST(ConnectionString, TheOperatorsSettingWins) {
    ConnectionString s("postgresql://u@h/db?connect_timeout=10");
    s.withDefault("connect_timeout", 5);
    EXPECT_EQ(s.text(), "postgresql://u@h/db?connect_timeout=10");
}

TEST(ConnectionString, MatchesWholeKeysOnly) {
    // "keepalives_idle" names neither "keepalives" nor "idle".
    ConnectionString s("host=h keepalives_idle=5");
    s.withDefault("keepalives", 1);
    EXPECT_EQ(s.text(), "host=h keepalives_idle=5 keepalives=1");
}

TEST(ConnectionString, AddsDefaultsToKeywords) {
    ConnectionString s("host=h dbname=db");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h dbname=db connect_timeout=2");
}

TEST(SessionFactory, BoundsEveryClientWait) {
    const std::string s =
        SessionFactory::withClientBounds("postgresql://u@h/db", {.connectTimeout = 2500ms, .applicationName = "x"});
    EXPECT_EQ(s,
              "postgresql://u@h/db?connect_timeout=3&keepalives=1&keepalives_idle=30&keepalives_interval=10"
              "&keepalives_count=3&tcp_user_timeout=60000&application_name=x");
}

TEST(SessionFactory, NeverGoesBelowTheLibpqMinimum) {
    const std::string s = SessionFactory::withClientBounds("host=h", {.connectTimeout = 100ms, .applicationName = ""});
    EXPECT_TRUE(s.starts_with("host=h connect_timeout=2 ")) << s;
    EXPECT_EQ(s.find("application_name"), std::string::npos);
}
