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

TEST(ConnectionString, SeesKeywordsWrittenWithBlanksAroundTheEquals) {
    ConnectionString s("host=h\tconnect_timeout = 10");
    s.withDefault("connect_timeout", 5);
    EXPECT_EQ(s.text(), "host=h\tconnect_timeout = 10");
}

TEST(ConnectionString, SeesKeywordsWrittenWithALineBreakBeforeTheEquals) {
    ConnectionString s("host=h\nconnect_timeout\n=10");
    s.withDefault("connect_timeout", 5);
    EXPECT_EQ(s.text(), "host=h\nconnect_timeout\n=10");
}

TEST(ConnectionString, DoesNotReadAKeywordInsideAQuotedValue) {
    ConnectionString s(R"(host=h application_name='api connect_timeout=5 it\'s' password=a\ b)");
    s.withDefault("connect_timeout", 2);
    s.withDefault("password", "x");
    EXPECT_EQ(s.text(), R"(host=h application_name='api connect_timeout=5 it\'s' password=a\ b connect_timeout=2)");
}

TEST(ConnectionString, ReadsAValueEndingInABackslash) {
    ConnectionString s(R"(host=h password=abc\)");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h password=abc connect_timeout=2");  // libpq drops the lone backslash
}

TEST(ConnectionString, QuotesAndEncodesTheValuesItAdds) {
    ConnectionString keywords("host=h");
    keywords.withDefault("application_name", "worker pool").withDefault("options", R"(it's \x)");
    EXPECT_EQ(keywords.text(), R"(host=h application_name='worker pool' options='it\'s \\x')");
    ConnectionString uri("postgresql://u@h/db");
    uri.withDefault("application_name", "worker pool&x=1");
    EXPECT_EQ(uri.text(), "postgresql://u@h/db?application_name=worker%20pool%26x%3D1");
}

TEST(ConnectionString, ReadsAnEmptyValueEndingInABackslash) {
    ConnectionString s(R"(host=h password=\)");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h password='' connect_timeout=2");
}

TEST(ConnectionString, QuotesAnEmptyLastValue) {
    ConnectionString s("host=h password= ");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h password= '' connect_timeout=2");
}

TEST(ConnectionString, LeavesABareValueEndingInAnEqualsSign) {
    ConnectionString s("host=h password=abc=");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h password=abc= connect_timeout=2");
}

TEST(ConnectionString, DoesNotQuoteTrailingBlanks) {
    ConnectionString s("host=h  ");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "host=h   connect_timeout=2");
}

TEST(ConnectionString, SeesUriParameterNamesWithBlanksAround) {
    ConnectionString s("postgresql://h/db? connect_timeout =10");
    s.withDefault("connect_timeout", 2);
    EXPECT_EQ(s.text(), "postgresql://h/db? connect_timeout =10");
}

TEST(ConnectionString, SeesPercentEncodedUriParameterNames) {
    ConnectionString s("postgresql://u@h/db?sslmode=require&connect%5Ftimeout=10");
    s.withDefault("connect_timeout", 5);
    EXPECT_EQ(s.text(), "postgresql://u@h/db?sslmode=require&connect%5Ftimeout=10");
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
