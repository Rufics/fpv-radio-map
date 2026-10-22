#pragma once

#include "admin_api.hpp"
#include "config.hpp"
#include "data_api.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "mission_api.hpp"
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

/// The HTTP server: the services, the three API controllers and the routes that lead to them.
/// Every handler runs inside dispatch(): rate limit, uniform JSON errors, request metrics.
class Application {
public:
    /// `ownerKeyText` is the built-in owner key (RFMAP_OWNER_PUBKEY_TEXT); `ownerKeyIsDev`
    /// says the build used the public development key, which the config must allow explicitly.
    Application(Config config, const std::string& ownerKeyText, bool ownerKeyIsDev, const std::filesystem::path& sqlDir);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Binds and serves until stop(). Returns false if the port could not be bound.
    bool listen();
    /// Binds to an ephemeral port on the configured address; returns it (for tests).
    int bindAny();
    void serveBound();
    void stop();

    [[nodiscard]] Logger& log() { return log_; }
    [[nodiscard]] const Config& config() const { return config_; }

    /// Bodies above this are refused before a handler sees them: nothing the API takes is larger.
    static constexpr std::size_t kMaxPayloadBytes = 1u << 20;

private:
    using Handler = std::function<void(const HttpRequest&, HttpReply&)>;
    using RawHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

    void configureServer();
    void routes();
    void missionRoutes(const std::string& api);
    void dataRoutes(const std::string& api);
    void adminRoutes(const std::string& api);
    /// The httplib handler that runs `handler` under the rate limit of `rateClass`, answers
    /// its errors as JSON and records the request in the metrics.
    [[nodiscard]] RawHandler route(std::string rateClass, Handler handler);
    /// Runs `handler`; every failure becomes a JSON answer.
    void dispatch(const std::string& rateClass, const Handler& handler, const httplib::Request& req,
                  httplib::Response& res);
    /// Logs an unexpected request failure and answers 500 without its details.
    void internalError(const httplib::Request& req, httplib::Response& res, std::string_view what);
    /// The Prometheus text of GET /admin/metrics.
    [[nodiscard]] std::string metricsText();

    /// A controller method as a Handler.
    template <typename Api>
    [[nodiscard]] Handler on(const Api& api, void (Api::*method)(const HttpRequest&, HttpReply&) const) {
        return [&api, method](const HttpRequest& req, HttpReply& res) { (api.*method)(req, res); };
    }

    Config config_;
    Logger log_;
    Services services_;
    AdminToken admin_;
    MissionApi missionApi_;
    DataApi dataApi_;
    AdminApi adminApi_;
    HttpMetrics httpMetrics_;  ///< before http_: its request threads report here until they stop
    double startTimeSeconds_ = 0.0;
    std::unique_ptr<httplib::Server> http_;
};

}  // namespace rfmap
