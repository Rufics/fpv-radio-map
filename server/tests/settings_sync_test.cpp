// The change notices server processes send each other, and the config of the settings sync.
// The listener and the poll against a database are in integration_test.cpp.

#include "config.hpp"
#include "settings_sync.hpp"

#include <gtest/gtest.h>

using namespace rfmap;

TEST(ChangeNotice, RoundTripsThroughItsPayload) {
    const ChangeNotice n(SettingsTopic::kBuiltins, "pod-a/xyz");
    const auto back = ChangeNotice::parse(n.payload());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->topic(), "builtins");
    EXPECT_EQ(back->origin(), "pod-a/xyz");
}

TEST(ChangeNotice, OtherTextOnTheChannelIsNotANotice) {
    EXPECT_FALSE(ChangeNotice::parse("").has_value());
    EXPECT_FALSE(ChangeNotice::parse("reload").has_value());
    EXPECT_FALSE(ChangeNotice::parse(R"({"topic": 3})").has_value());
    EXPECT_FALSE(ChangeNotice::parse(R"(["builtins"])").has_value());
    // Without an origin it is still a notice, from no process this one knows.
    const auto anonymous = ChangeNotice::parse(R"({"topic": "all"})");
    ASSERT_TRUE(anonymous.has_value());
    EXPECT_EQ(anonymous->origin(), "");
}

TEST(SettingsSyncConfig, DefaultsListenAndPollEveryMinute) {
    const Config c = Config::fromJson(json::object());
    EXPECT_TRUE(c.settingsSync.listen);
    EXPECT_EQ(c.settingsSync.pollSeconds, 60);
    EXPECT_EQ(c.database.listenUrl, "");
}

TEST(SettingsSyncConfig, ReadsItsKeysAndHidesTheListenUrl) {
    const Config c = Config::fromJson(
        {{"database", {{"url", "postgresql://a:secret@pgbouncer/rfmap"}, {"listenUrl", "postgresql://a:secret@pg/rfmap"}}},
         {"settingsSync", {{"listen", false}, {"pollSeconds", 15}}}});
    EXPECT_FALSE(c.settingsSync.listen);
    EXPECT_EQ(c.settingsSync.pollSeconds, 15);
    EXPECT_EQ(c.database.listenUrl, "postgresql://a:secret@pg/rfmap");
    EXPECT_EQ(c.raw["database"]["listenUrl"], "(set)");
    EXPECT_EQ(c.raw["database"]["url"], "(set)");
}

TEST(SettingsSyncConfig, EnvironmentReachesTheNewKeys) {
    const Config c = Config::load("", {{"RFMAP__SETTINGSSYNC__POLLSECONDS", "30"},
                                       {"RFMAP__DATABASE__LISTENURL", "postgresql://direct/rfmap"}});
    EXPECT_EQ(c.settingsSync.pollSeconds, 30);
    EXPECT_EQ(c.database.listenUrl, "postgresql://direct/rfmap");
}

TEST(SettingsSyncConfig, RefusesAPollOutOfRange) {
    EXPECT_THROW((void)Config::fromJson({{"settingsSync", {{"pollSeconds", 0}}}}), std::invalid_argument);
    EXPECT_THROW((void)Config::fromJson({{"settingsSync", {{"pollSeconds", 7200}}}}), std::invalid_argument);
}
