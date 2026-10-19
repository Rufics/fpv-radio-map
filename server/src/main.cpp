// rfmap-server: the FPV Radio Map HTTP API.
//
//   rfmap-server [--config /etc/rfmap/server.json] [--sql /usr/share/rfmap/sql]
//   rfmap-server --health [http://127.0.0.1:8080]   exit 0 if /api/v1/health answers 200
//
// The config file is optional (RFMAP_CONFIG names it too); environment variables override it.

#include "app.hpp"

#include <httplib.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
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

/// What the command line asks for: the files to run with, or an exit code to return at once.
struct CommandLine {
    std::filesystem::path configFile;
    std::filesystem::path sqlDir = RFMAP_DEFAULT_SQL_DIR;
    std::optional<int> exitCode;
};

CommandLine parseCommandLine(const std::vector<std::string_view>& args) {
    CommandLine out;
    if (const char* c = std::getenv("RFMAP_CONFIG")) out.configFile = c;
    if (const char* s = std::getenv("RFMAP_SQL_DIR")) out.sqlDir = s;
    std::size_t i = 0;
    while (i < args.size() && !out.exitCode.has_value()) {
        const std::string_view a = args[i++];
        const bool hasValue = i < args.size();
        if (a == "--config" && hasValue) {
            out.configFile = args[i++];
        } else if (a == "--sql" && hasValue) {
            out.sqlDir = args[i++];
        } else if (a == "--health") {
            out.exitCode = health(hasValue ? std::string(args[i]) : "http://127.0.0.1:8080");
        } else if (a == "--help" || a == "-h") {
            std::cout << "usage: rfmap-server [--config file.json] [--sql dir]\n"
                         "       rfmap-server --health [http://127.0.0.1:8080]\n";
            out.exitCode = 0;
        } else {
            std::cerr << "unknown argument " << a << "\n";
            out.exitCode = 2;
        }
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const CommandLine cmd = parseCommandLine(args);
    if (cmd.exitCode.has_value()) return *cmd.exitCode;
    try {
        rfmap::Application app(rfmap::Config::load(cmd.configFile, rfmap::Config::processEnvironment()),
                               RFMAP_OWNER_PUBKEY_TEXT, RFMAP_OWNER_KEY_IS_DEV != 0, cmd.sqlDir);
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
