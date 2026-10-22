#pragma once

#include "config.hpp"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// The rfmap-server command line: the files to run with, or what to do instead of serving.
class CommandLine {
public:
    /// Backup, Restore and Migrate are service actions: they read the configuration like Serve
    /// does, so --config and --sql may come before or after them.
    enum class Action { Serve, Health, Help, Invalid, Backup, Restore, Migrate };

    static constexpr std::string_view kUsage =
        "usage: rfmap-server [--config file.json] [--sql dir]\n"
        "       rfmap-server --health [http://127.0.0.1:8080]\n"
        "       rfmap-server [--config file.json] [--sql dir] --backup  > rfmap.backup\n"
        "       rfmap-server [--config file.json] [--sql dir] --restore < rfmap.backup\n"
        "       rfmap-server [--config file.json] [--sql dir] --migrate [--plan]\n";
    static constexpr std::string_view kDefaultHealthUrl = "http://127.0.0.1:8080";

    /// Reads `args` (without the program name). RFMAP_CONFIG and RFMAP_SQL_DIR give the defaults
    /// that --config and --sql override; the first --health, --help or unknown argument decides,
    /// and a second service action is an invalid argument, as is --plan without --migrate.
    static CommandLine parse(const std::vector<std::string_view>& args, const Config::Environment& env,
                             std::filesystem::path defaultSqlDir);

    [[nodiscard]] Action action() const { return action_; }
    /// True for the actions that need the configuration and the database (Backup, Restore, Migrate).
    [[nodiscard]] bool isServiceAction() const {
        return action_ == Action::Backup || action_ == Action::Restore || action_ == Action::Migrate;
    }
    /// For Action::Migrate: --plan, which prints the steps instead of applying them.
    [[nodiscard]] bool planOnly() const { return planOnly_; }
    [[nodiscard]] const std::filesystem::path& configFile() const { return configFile_; }
    [[nodiscard]] const std::filesystem::path& sqlDir() const { return sqlDir_; }
    /// The server --health asks, for Action::Health.
    [[nodiscard]] const std::string& healthUrl() const { return healthUrl_; }
    /// The argument that was not understood, for Action::Invalid.
    [[nodiscard]] const std::string& invalidArgument() const { return invalidArgument_; }

    /// Answers Health, Help and Invalid, which need no configuration; returns the process exit code.
    int answerWithoutServing(std::ostream& out, std::ostream& err) const;

private:
    /// The service action an argument names, or Invalid.
    static Action serviceAction(std::string_view arg);
    /// For container health checks (the runtime image has no curl): 0 when /api/v1/health answers 200.
    static int health(const std::string& base, std::ostream& err);

    Action action_ = Action::Serve;
    std::filesystem::path configFile_;
    std::filesystem::path sqlDir_;
    std::string healthUrl_;
    std::string invalidArgument_;
    bool planOnly_ = false;
};

}  // namespace rfmap
