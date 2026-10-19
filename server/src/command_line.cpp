#include "command_line.hpp"

#include <httplib.h>

#include <format>

namespace rfmap {

CommandLine CommandLine::parse(const std::vector<std::string_view>& args, const Config::Environment& env,
                               std::filesystem::path defaultSqlDir) {
    using enum Action;
    CommandLine out;
    out.sqlDir_ = std::move(defaultSqlDir);
    if (const auto c = env.find("RFMAP_CONFIG"); c != env.end()) out.configFile_ = c->second;
    if (const auto s = env.find("RFMAP_SQL_DIR"); s != env.end()) out.sqlDir_ = s->second;
    std::size_t i = 0;
    while (i < args.size() && out.action_ == Serve) {
        const std::string_view a = args[i++];
        const bool hasValue = i < args.size();
        if (a == "--config" && hasValue) {
            out.configFile_ = args[i++];
        } else if (a == "--sql" && hasValue) {
            out.sqlDir_ = args[i++];
        } else if (a == "--health") {
            out.action_ = Health;
            out.healthUrl_ = hasValue ? args[i] : kDefaultHealthUrl;
        } else if (a == "--help" || a == "-h") {
            out.action_ = Help;
        } else {
            out.action_ = Invalid;
            out.invalidArgument_ = a;
        }
    }
    return out;
}

int CommandLine::answerWithoutServing(std::ostream& out, std::ostream& err) const {
    using enum Action;
    switch (action_) {
        case Health: return health(healthUrl_, err);
        case Help: out << kUsage; return 0;
        case Invalid: err << "unknown argument " << invalidArgument_ << "\n"; return 2;
        case Serve: break;
    }
    return 0;
}

int CommandLine::health(const std::string& base, std::ostream& err) {
    httplib::Client client(base);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto res = client.Get("/api/v1/health");
    if (res && res->status == 200) return 0;
    err << "rfmap-server: health check failed" << (res ? std::format(" with HTTP {}", res->status) : "") << "\n";
    return 1;
}

}  // namespace rfmap
