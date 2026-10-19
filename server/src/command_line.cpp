#include "command_line.hpp"

namespace rfmap {

CommandLine CommandLine::parse(const std::vector<std::string_view>& args, const Environment& env,
                               std::filesystem::path defaultSqlDir) {
    CommandLine out;
    out.sqlDir_ = std::move(defaultSqlDir);
    if (const char* c = env("RFMAP_CONFIG")) out.configFile_ = c;
    if (const char* s = env("RFMAP_SQL_DIR")) out.sqlDir_ = s;
    std::size_t i = 0;
    while (i < args.size() && out.action_ == Action::Serve) {
        const std::string_view a = args[i++];
        const bool hasValue = i < args.size();
        if (a == "--config" && hasValue) {
            out.configFile_ = args[i++];
        } else if (a == "--sql" && hasValue) {
            out.sqlDir_ = args[i++];
        } else if (a == "--health") {
            out.action_ = Action::Health;
            out.healthUrl_ = hasValue ? args[i] : kDefaultHealthUrl;
        } else if (a == "--help" || a == "-h") {
            out.action_ = Action::Help;
        } else {
            out.action_ = Action::Invalid;
            out.invalidArgument_ = a;
        }
    }
    return out;
}

}  // namespace rfmap
