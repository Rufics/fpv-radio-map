// rfmap-server: the FPV Radio Map HTTP API.
//
//   rfmap-server [--config /etc/rfmap/server.json] [--sql /usr/share/rfmap/sql]
//   rfmap-server --health [http://127.0.0.1:8080]   exit 0 if /api/v1/health answers 200
//
// The config file is optional (RFMAP_CONFIG names it too); environment variables override it.

#include "app.hpp"
#include "command_line.hpp"

#include <sys/resource.h>

#include <atomic>
#include <csignal>
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

/// What the process does to itself before it holds secrets: no core dumps, since memory holds
/// the admin token and session hashes. (Not PR_SET_DUMPABLE: that hands /proc/self to root,
/// and the metrics read the process's own fd table there.)
class ProcessHardening {
public:
    /// False when the limit could not be set; the server still starts.
    [[nodiscard]] static bool apply() {
        const rlimit none{0, 0};
        return setrlimit(RLIMIT_CORE, &none) == 0;
    }
};
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const auto env = rfmap::Config::processEnvironment();
    const auto cmd = rfmap::CommandLine::parse(args, env, RFMAP_DEFAULT_SQL_DIR);
    if (cmd.action() != rfmap::CommandLine::Action::Serve) return cmd.answerWithoutServing(std::cout, std::cerr);
    try {
        const bool hardened = ProcessHardening::apply();
        rfmap::Application app(rfmap::Config::load(cmd.configFile(), env),
                               RFMAP_OWNER_PUBKEY_TEXT, RFMAP_OWNER_KEY_IS_DEV != 0, cmd.sqlDir());
        if (!hardened) app.log().warn("process", "could not disable core dumps (setrlimit RLIMIT_CORE failed)");
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
