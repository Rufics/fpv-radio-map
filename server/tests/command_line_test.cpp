#include "command_line.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using rfmap::CommandLine;
using Action = rfmap::CommandLine::Action;

namespace {

/// A process environment backed by a map, so tests never read the real one.
class FakeEnvironment {
public:
    FakeEnvironment& set(const std::string& name, const std::string& value) {
        vars_[name] = value;
        return *this;
    }

    [[nodiscard]] CommandLine::Environment lookup() const {
        return [this](const char* name) -> const char* {
            const auto it = vars_.find(name);
            return it == vars_.end() ? nullptr : it->second.c_str();
        };
    }

private:
    std::map<std::string, std::string, std::less<>> vars_;
};

class CommandLineTest : public ::testing::Test {
protected:
    CommandLine parse(const std::vector<std::string_view>& args) const {
        return CommandLine::parse(args, env.lookup(), "/usr/share/rfmap/sql");
    }

    FakeEnvironment env;
};

}  // namespace

TEST_F(CommandLineTest, NoArgumentsServesWithTheDefaults) {
    const auto cl = parse({});
    EXPECT_EQ(cl.action(), Action::Serve);
    EXPECT_TRUE(cl.configFile().empty());
    EXPECT_EQ(cl.sqlDir(), "/usr/share/rfmap/sql");
}

TEST_F(CommandLineTest, EnvironmentGivesTheDefaults) {
    env.set("RFMAP_CONFIG", "/etc/rfmap.json").set("RFMAP_SQL_DIR", "/opt/sql");
    const auto cl = parse({});
    EXPECT_EQ(cl.action(), Action::Serve);
    EXPECT_EQ(cl.configFile(), "/etc/rfmap.json");
    EXPECT_EQ(cl.sqlDir(), "/opt/sql");
}

TEST_F(CommandLineTest, FlagsOverrideTheEnvironment) {
    env.set("RFMAP_CONFIG", "/etc/rfmap.json").set("RFMAP_SQL_DIR", "/opt/sql");
    const auto cl = parse({"--config", "local.json", "--sql", "sql"});
    EXPECT_EQ(cl.action(), Action::Serve);
    EXPECT_EQ(cl.configFile(), "local.json");
    EXPECT_EQ(cl.sqlDir(), "sql");
}

TEST_F(CommandLineTest, HealthUsesTheDefaultUrl) {
    const auto cl = parse({"--health"});
    EXPECT_EQ(cl.action(), Action::Health);
    EXPECT_EQ(cl.healthUrl(), CommandLine::kDefaultHealthUrl);
}

TEST_F(CommandLineTest, HealthTakesAnUrl) {
    const auto cl = parse({"--config", "c.json", "--health", "http://10.0.0.2:9000"});
    EXPECT_EQ(cl.action(), Action::Health);
    EXPECT_EQ(cl.healthUrl(), "http://10.0.0.2:9000");
    EXPECT_EQ(cl.configFile(), "c.json");
}

TEST_F(CommandLineTest, HelpInEitherSpelling) {
    EXPECT_EQ(parse({"--help"}).action(), Action::Help);
    EXPECT_EQ(parse({"-h"}).action(), Action::Help);
}

TEST_F(CommandLineTest, UnknownArgumentIsInvalid) {
    const auto cl = parse({"--sql", "sql", "--verbose", "--help"});
    EXPECT_EQ(cl.action(), Action::Invalid);
    EXPECT_EQ(cl.invalidArgument(), "--verbose");
}

TEST_F(CommandLineTest, FlagWithoutItsValueIsInvalid) {
    const auto config = parse({"--config"});
    EXPECT_EQ(config.action(), Action::Invalid);
    EXPECT_EQ(config.invalidArgument(), "--config");
    EXPECT_EQ(parse({"--sql"}).action(), Action::Invalid);
}
