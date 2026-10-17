// rfmap-server: the FPV Radio Map HTTP API.
//
//   rfmap-server [--config /etc/rfmap/server.json] [--sql /usr/share/rfmap/sql]
//   rfmap-server --health [http://127.0.0.1:8080]   exit 0 if /api/v1/health answers 200
//
// The config file is optional (RFMAP_CONFIG names it too); environment variables override it.

#include "app.hpp"

#include <httplib.h>

#include <csignal>
#include <iostream>

namespace {
rfmap::Application* running = nullptr;

void onSignal(int) {
    if (running) running->stop();
}

/// For container health checks: the runtime image has no curl.
int health(const std::string& base) {
    httplib::Client client(base);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto res = client.Get("/api/v1/health");
    if (res && res->status == 200) return 0;
    std::cerr << "rfmap-server: health check failed" << (res ? " with HTTP " + std::to_string(res->status) : "") << "\n";
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path configFile;
    std::filesystem::path sqlDir = RFMAP_DEFAULT_SQL_DIR;
    if (const char* c = std::getenv("RFMAP_CONFIG")) configFile = c;
    if (const char* s = std::getenv("RFMAP_SQL_DIR")) sqlDir = s;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) configFile = argv[++i];
        else if (a == "--sql" && i + 1 < argc) sqlDir = argv[++i];
        else if (a == "--health") return health(i + 1 < argc ? argv[i + 1] : "http://127.0.0.1:8080");
        else if (a == "--help" || a == "-h") {
            std::cout << "usage: rfmap-server [--config file.json] [--sql dir]\n"
                         "       rfmap-server --health [http://127.0.0.1:8080]\n";
            return 0;
        } else {
            std::cerr << "unknown argument " << a << "\n";
            return 2;
        }
    }
    try {
        rfmap::Application app(rfmap::Config::load(configFile, rfmap::Config::processEnvironment()),
                               RFMAP_OWNER_PUBKEY_TEXT, RFMAP_OWNER_KEY_IS_DEV != 0, sqlDir);
        running = &app;
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT, onSignal);
        std::signal(SIGPIPE, SIG_IGN);
        const bool ok = app.listen();
        running = nullptr;
        if (!ok) {
            app.log().error("http", "could not listen on " + app.config().server.listen + ":" +
                                        std::to_string(app.config().server.port));
            return 1;
        }
        app.log().info("http", "stopped");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "rfmap-server: " << e.what() << std::endl;
        return 1;
    }
}
