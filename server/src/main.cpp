// rfmap-server: the FPV Radio Map HTTP API.
//
//   rfmap-server [--config /etc/rfmap/server.json] [--sql /usr/share/rfmap/sql]
//   rfmap-server --health [http://127.0.0.1:8080]   exit 0 if /api/v1/health answers 200
//
// The config file is optional (RFMAP_CONFIG names it too); environment variables override it.

#include "app.hpp"
#include "command_line.hpp"

#include <httplib.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
/// The application a signal stops. Constant-initialised and lock-free, so the handler may read it.
std::atomic<rfmap::Application*>& runningApplication() {
    static std::atomic<rfmap::Application*> running{nullptr};
    return running;
}

void onSignal(int) {
    if (rfmap::Application* app = runningApplication().load()) app->stop();
}

/// For container health checks: the runtime image has no curl.
int health(const std::string& base) {
    httplib::Client client(base);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto res = client.Get("/api/v1/health");
    if (res && res->status == 200) return 0;
    std::cerr << "rfmap-server: health check failed" << (res ? std::format(" with HTTP {}", res->status) : "") << "\n";
    return 1;
}

/// Does what the command line asks instead of serving; returns the process exit code.
int answerWithoutServing(const rfmap::CommandLine& cmd) {
    using enum rfmap::CommandLine::Action;
    switch (cmd.action()) {
        case Health: return health(cmd.healthUrl());
        case Help: std::cout << rfmap::CommandLine::kUsage; return 0;
        case Invalid: std::cerr << "unknown argument " << cmd.invalidArgument() << "\n"; return 2;
        case Serve: break;
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const auto cmd = rfmap::CommandLine::parse(args, [](const char* name) { return std::getenv(name); },
                                               RFMAP_DEFAULT_SQL_DIR);
    if (cmd.action() != rfmap::CommandLine::Action::Serve) return answerWithoutServing(cmd);
    try {
        rfmap::Application app(rfmap::Config::load(cmd.configFile(), rfmap::Config::processEnvironment()),
                               RFMAP_OWNER_PUBKEY_TEXT, RFMAP_OWNER_KEY_IS_DEV != 0, cmd.sqlDir());
        runningApplication() = &app;
        (void)std::signal(SIGTERM, onSignal);
        (void)std::signal(SIGINT, onSignal);
        (void)std::signal(SIGPIPE, SIG_IGN);
        const bool ok = app.listen();
        runningApplication() = nullptr;
        if (!ok) {
            const auto& server = app.config().server;
            app.log().error("http", std::format("could not listen on {}:{}", server.listen, server.port));
            return 1;
        }
        app.log().info("http", "stopped");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "rfmap-server: " << e.what() << '\n';
        return 1;
    }
}
