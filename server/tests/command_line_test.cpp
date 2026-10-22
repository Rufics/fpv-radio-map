#include "command_line.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using rfmap::CommandLine;
using Action = rfmap::CommandLine::Action;

namespace {

class CommandLineTest : public ::testing::Test {
protected:
    CommandLine parse(const std::vector<std::string_view>& args) const {
        return CommandLine::parse(args, env, "/usr/share/rfmap/sql");
    }

    /// Runs what `args` ask instead of serving; returns the exit code.
    int answer(const std::vector<std::string_view>& args) {
        out.str("");
        err.str("");
        return parse(args).answerWithoutServing(out, err);
    }

    rfmap::Config::Environment env;
    std::ostringstream out;
    std::ostringstream err;
};

/// A server whose /api/v1/health answers with the status it is given.
class HealthEndpoint {
public:
    explicit HealthEndpoint(int status) {
        server_.Get("/api/v1/health", [status](const httplib::Request&, httplib::Response& res) {
            res.status = status;
            res.set_content("{}", "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~HealthEndpoint() {
        server_.stop();
        thread_.join();
    }
    HealthEndpoint(const HealthEndpoint&) = delete;
    HealthEndpoint& operator=(const HealthEndpoint&) = delete;

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
};

}  // namespace

TEST_F(CommandLineTest, NoArgumentsServesWithTheDefaults) {
    const auto cl = parse({});
    EXPECT_EQ(cl.action(), Action::Serve);
    EXPECT_TRUE(cl.configFile().empty());
    EXPECT_EQ(cl.sqlDir(), "/usr/share/rfmap/sql");
    EXPECT_EQ(cl.answerWithoutServing(out, err), 0);
}

TEST_F(CommandLineTest, EnvironmentGivesTheDefaults) {
    env = {{"RFMAP_CONFIG", "/etc/rfmap.json"}, {"RFMAP_SQL_DIR", "/opt/sql"}};
    const auto cl = parse({});
    EXPECT_EQ(cl.action(), Action::Serve);
    EXPECT_EQ(cl.configFile(), "/etc/rfmap.json");
    EXPECT_EQ(cl.sqlDir(), "/opt/sql");
}

TEST_F(CommandLineTest, FlagsOverrideTheEnvironment) {
    env = {{"RFMAP_CONFIG", "/etc/rfmap.json"}, {"RFMAP_SQL_DIR", "/opt/sql"}};
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

TEST_F(CommandLineTest, HelpInEitherSpellingPrintsTheUsage) {
    EXPECT_EQ(parse({"--help"}).action(), Action::Help);
    EXPECT_EQ(answer({"-h"}), 0);
    EXPECT_EQ(out.str(), CommandLine::kUsage);
}

TEST_F(CommandLineTest, UnknownArgumentIsInvalid) {
    const auto cl = parse({"--sql", "sql", "--verbose", "--help"});
    EXPECT_EQ(cl.action(), Action::Invalid);
    EXPECT_EQ(cl.invalidArgument(), "--verbose");
    EXPECT_EQ(answer({"--verbose"}), 2);
    EXPECT_EQ(err.str(), "unknown argument --verbose\n");
}

TEST_F(CommandLineTest, FlagWithoutItsValueIsInvalid) {
    const auto config = parse({"--config"});
    EXPECT_EQ(config.action(), Action::Invalid);
    EXPECT_EQ(config.invalidArgument(), "--config");
    EXPECT_EQ(parse({"--sql"}).action(), Action::Invalid);
}

TEST_F(CommandLineTest, HealthCheckPassesOnlyOnHttp200) {
    const HealthEndpoint healthy(200);
    const std::string healthyUrl = healthy.url();
    EXPECT_EQ(answer({"--health", healthyUrl}), 0);
    EXPECT_TRUE(err.str().empty());

    const HealthEndpoint failing(503);
    const std::string failingUrl = failing.url();
    EXPECT_EQ(answer({"--health", failingUrl}), 1);
    EXPECT_EQ(err.str(), "rfmap-server: health check failed with HTTP 503\n");
}

TEST_F(CommandLineTest, HealthCheckFailsWhenNothingAnswers) {
    std::string url;
    {
        const HealthEndpoint gone(200);
        url = gone.url();
    }
    EXPECT_EQ(answer({"--health", url}), 1);
    EXPECT_EQ(err.str(), "rfmap-server: health check failed\n");
}

TEST_F(CommandLineTest, BackupAndRestoreReadTheFilesOnEitherSide) {
    env = {{"RFMAP_SQL_DIR", "/opt/sql"}};
    const auto backup = parse({"--config", "c.json", "--backup"});
    EXPECT_EQ(backup.action(), Action::Backup);
    EXPECT_TRUE(backup.isServiceAction());
    EXPECT_EQ(backup.configFile(), "c.json");
    const auto restore = parse({"--restore", "--sql", "sql", "--config", "d.json"});
    EXPECT_EQ(restore.action(), Action::Restore);
    EXPECT_EQ(restore.sqlDir(), "sql");
    EXPECT_EQ(restore.configFile(), "d.json");
    EXPECT_FALSE(parse({}).isServiceAction());
}

TEST_F(CommandLineTest, OneServiceActionAtATime) {
    const auto both = parse({"--backup", "--restore"});
    EXPECT_EQ(both.action(), Action::Invalid);
    EXPECT_EQ(both.invalidArgument(), "--restore");
    EXPECT_EQ(parse({"--restore", "--health"}).action(), Action::Invalid);
    EXPECT_EQ(parse({"--backup", "--sql"}).action(), Action::Invalid);
}
