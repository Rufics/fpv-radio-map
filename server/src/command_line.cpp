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
    while (i < args.size() && (out.action_ == Serve || out.isServiceAction())) {
        const std::string_view a = args[i++];
        const bool hasValue = i < args.size();
        if (a == "--config" && hasValue) {
            out.configFile_ = args[i++];
        } else if (a == "--sql" && hasValue) {
            out.sqlDir_ = args[i++];
        } else if (const Action service = serviceAction(a); service != Invalid && out.action_ == Serve) {
            out.action_ = service;
        } else if (a == "--plan" && !out.planOnly_) {
            out.planOnly_ = true;
        } else if (a == "--health" && out.action_ == Serve) {
            out.action_ = Health;
            out.healthUrl_ = hasValue ? args[i] : kDefaultHealthUrl;
        } else if ((a == "--help" || a == "-h") && out.action_ == Serve) {
            out.action_ = Help;
        } else {
            out.action_ = Invalid;
            out.invalidArgument_ = a;
        }
    }
    out.requirePlanWithMigrate();
    return out;
}

void CommandLine::requirePlanWithMigrate() {
    using enum Action;
    if (planOnly_ && (action_ == Serve || isServiceAction()) && action_ != Migrate) {
        action_ = Invalid;
        invalidArgument_ = "--plan (it goes with --migrate)";
    }
}

CommandLine::Action CommandLine::serviceAction(std::string_view arg) {
    using enum Action;
    if (arg == "--backup") return Backup;
    if (arg == "--restore") return Restore;
    if (arg == "--migrate") return Migrate;
    return Invalid;
}

int CommandLine::answerWithoutServing(std::ostream& out, std::ostream& err) const {
    using enum Action;
    switch (action_) {
        case Health: return health(healthUrl_, err);
        case Help: out << kUsage; return 0;
        case Invalid: err << "unknown argument " << invalidArgument_ << "\n"; return 2;
        case Serve:
        case Backup:
        case Restore:
        case Migrate: break;
    }
    return 0;
}

std::pair<std::string, std::string> CommandLine::splitUrl(std::string_view url) {
    const auto scheme = url.find("://");
    const auto hostStart = scheme == std::string_view::npos ? 0 : scheme + 3;
    const auto slash = url.find('/', hostStart);
    if (slash == std::string_view::npos || slash + 1 == url.size()) {
        return {std::string(url.substr(0, slash)), std::string(kDefaultHealthPath)};
    }
    return {std::string(url.substr(0, slash)), std::string(url.substr(slash))};
}

int CommandLine::health(const std::string& url, std::ostream& err) {
    const auto [origin, path] = splitUrl(url);
    httplib::Client client(origin);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto res = client.Get(path);
    if (res && res->status == 200) return 0;
    err << "rfmap-server: health check failed" << (res ? std::format(" with HTTP {}", res->status) : "") << "\n";
    return 1;
}

}  // namespace rfmap
