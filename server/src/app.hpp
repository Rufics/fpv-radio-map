#pragma once

#include "admin_api.hpp"
#include "config.hpp"
#include "data_api.hpp"
#include "flight_log_api.hpp"
#include "lifecycle.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "mission_api.hpp"
#include "ops_server.hpp"
#include "readiness.hpp"
#include "request.hpp"
#include "services.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace rfmap {

/// The HTTP server: the services, the four API controllers and the routes that lead to them.
/// Every handler runs inside dispatch(): rate limit, uniform JSON errors, request metrics.
/// Beside it, the operations listener (ops_server.hpp) answers health, readiness and metrics.
class Application {
public:
    /// `ownerKeyText` is the built-in owner key (RFMAP_OWNER_PUBKEY_TEXT); `ownerKeyIsDev`
    /// says the build used the public development key, which the config must allow explicitly.
    Application(Config config, const std::string& ownerKeyText, bool ownerKeyIsDev, const std::filesystem::path& sqlDir);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Binds the API and, when enabled, the operations listener, and serves until stop() or a
    /// shutdown request has run its course. Returns false if a port could not be bound.
    bool listen();
    /// Binds the API to an ephemeral port on the configured address; returns it (for tests).
    int bindAny();
    /// Binds the operations listener to an ephemeral port; returns it (for tests).
    int bindAnyOps();
    void serveBound();
    /// Stops at once: the listening sockets close and the requests in flight finish.
    void stop();
    /// Begins the graceful stop: /readyz answers 503, the server keeps serving for
    /// ops.drainSeconds, then stop(). A second request (a second Ctrl-C) cuts the drain short.
    /// Safe from a signal handler: it only sets a flag that the stop thread polls.
    void requestShutdown() noexcept { stopper_->request(); }

    [[nodiscard]] Logger& log() { return log_; }
    [[nodiscard]] const Config& config() const { return config_; }

private:
    using Handler = std::function<void(const HttpRequest&, HttpReply&)>;
    using RawHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

    void configureServer();
    void routes();
    void missionRoutes(const std::string& api);
    void flightLogRoutes(const std::string& api);
    void dataRoutes(const std::string& api);
    void adminRoutes(const std::string& api);
    /// The httplib handler that runs `handler` under the rate limit of `rateClass`, answers
    /// its errors as JSON and records the request in the metrics.
    [[nodiscard]] RawHandler route(std::string rateClass, Handler handler);
    /// Runs `handler`; every failure becomes a JSON answer.
    template <typename Fn>
    void dispatch(const std::string& rateClass, const Fn& handler, const httplib::Request& req, httplib::Response& res);
    /// Logs an unexpected request failure and answers 500 without its details.
    void internalError(const httplib::Request& req, httplib::Response& res, std::string_view what);
    /// The Prometheus text of GET /metrics on the operations listener.
    [[nodiscard]] std::string metricsText() const;
    /// The operations listener with this server's readiness checks and metrics attached.
    std::unique_ptr<OpsServer> makeOpsServer();

    /// A controller method as a Handler.
    template <typename Api, typename Method>
    [[nodiscard]] static Handler on(const Api& api, Method method) {
        return [&api, method](const HttpRequest& req, HttpReply& res) { (api.*method)(req, res); };
    }

    Config config_;
    Logger log_;
    Services services_;
    AdminToken admin_;
    MissionApi missionApi_;
    FlightLogApi flightLogApi_;
    DataApi dataApi_;
    AdminApi adminApi_;
    HttpMetrics httpMetrics_;  ///< before http_: its request threads report here until they stop
    double startTimeSeconds_ = 0.0;
    std::unique_ptr<httplib::Server> http_;
    std::unique_ptr<DatabaseProbe> dbProbe_;
    std::unique_ptr<OpsServer> ops_;  ///< reads the services above for /readyz and /metrics: destroyed first
    std::unique_ptr<GracefulStop> stopper_;
};

}  // namespace rfmap
