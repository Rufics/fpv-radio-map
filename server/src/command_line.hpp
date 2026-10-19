#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// The rfmap-server command line: the files to run with, or what to do instead of serving.
class CommandLine {
public:
    enum class Action { Serve, Health, Help, Invalid };
    /// Looks up an environment variable; null when it is not set.
    using Environment = std::function<const char*(const char*)>;

    static constexpr std::string_view kUsage =
        "usage: rfmap-server [--config file.json] [--sql dir]\n"
        "       rfmap-server --health [http://127.0.0.1:8080]\n";
    static constexpr std::string_view kDefaultHealthUrl = "http://127.0.0.1:8080";

    /// Reads `args` (without the program name). RFMAP_CONFIG and RFMAP_SQL_DIR give the defaults
    /// that --config and --sql override; the first --health, --help or unknown argument decides.
    static CommandLine parse(const std::vector<std::string_view>& args, const Environment& env,
                             std::filesystem::path defaultSqlDir);

    [[nodiscard]] Action action() const { return action_; }
    [[nodiscard]] const std::filesystem::path& configFile() const { return configFile_; }
    [[nodiscard]] const std::filesystem::path& sqlDir() const { return sqlDir_; }
    /// The server --health asks, for Action::Health.
    [[nodiscard]] const std::string& healthUrl() const { return healthUrl_; }
    /// The argument that was not understood, for Action::Invalid.
    [[nodiscard]] const std::string& invalidArgument() const { return invalidArgument_; }

private:
    Action action_ = Action::Serve;
    std::filesystem::path configFile_;
    std::filesystem::path sqlDir_;
    std::string healthUrl_;
    std::string invalidArgument_;
};

}  // namespace rfmap
