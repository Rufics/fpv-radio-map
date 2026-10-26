#pragma once

#include "auth.hpp"
#include "builtins.hpp"
#include "calculator.hpp"
#include "config.hpp"
#include "database.hpp"
#include "host_services.hpp"
#include "jobs.hpp"
#include "library.hpp"
#include "lifecycle.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "missions.hpp"
#include "module_runner.hpp"
#include "ops_server.hpp"
#include "pg_auth_store.hpp"
#include "rate_limiter.hpp"
#include "raster.hpp"
#include "readiness.hpp"
#include "result_cache.hpp"
#include "schemas.hpp"
#include "weather.hpp"

#include <rfmod/registry.hpp>
#include <rfmod/trust.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace rfmap {

/// The whole server: owns every service in dependency order and the HTTP routes.
class Application {
public:
    /// `ownerKeyText` is the built-in owner key (RFMAP_OWNER_PUBKEY_TEXT); `ownerKeyIsDev`
    /// says the build used the public development key, which is logged as a warning.
    Application(Config config, const std::string& ownerKeyText, bool ownerKeyIsDev, const std::filesystem::path& sqlDir);
    ~Application();

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
    // The route handlers, grouped by area; each is built per request around *this.
    struct MissionApi;
    struct DataApi;
    struct AdminApi;

    void routes();
    /// Runs `run` under the rate limit of `rateClass` and answers its errors as JSON.
    template <typename Run>
    void dispatch(const std::string& rateClass, const Run& run, const httplib::Request& req, httplib::Response& res);
    /// The HTTP handler that runs `method` of a fresh `Api` inside dispatch.
    template <typename Api, typename Method>
    auto route(std::string rateClass, Method method);
    /// Logs an unexpected request failure and answers 500 without its details.
    void internalError(const httplib::Request& req, httplib::Response& res, std::string_view what);
    /// The PIN hashing gate for this config, its limits logged.
    std::unique_ptr<PinGate> makePinGate();
    void loadCacheSettings() const;
    [[nodiscard]] json cacheTiers() const;
    /// The Prometheus text of GET /metrics on the operations listener.
    [[nodiscard]] std::string metricsText();
    /// The operations listener with this server's readiness checks and metrics attached.
    std::unique_ptr<OpsServer> makeOpsServer();
    json moduleList(bool admin);
    json mapConfig() const;

    // request helpers
    [[nodiscard]] std::string client(const httplib::Request& req) const;
    [[nodiscard]] json body(const httplib::Request& req) const;
    [[nodiscard]] Principal session(const httplib::Request& req) const;
    void requireAdmin(const httplib::Request& req);
    [[nodiscard]] bool isAdmin(const httplib::Request& req) const;
    /// The library kind of a path, checked: HttpError 404 for an unknown or disabled kind.
    [[nodiscard]] std::string libraryKind(const std::string& kind) const;
    [[nodiscard]] json trustedKeys() const;

    /// Persistent state: the database and the stores and services built on it.
    struct Stores {
        std::unique_ptr<Database> db;
        std::unique_ptr<SchemaRegistry> schemas;
        std::unique_ptr<PgAuthStore> authStore;
        std::unique_ptr<PinHasher> pins;
        std::unique_ptr<PinGate> pinGate;
        std::unique_ptr<AuthService> auth;
        std::unique_ptr<MissionStore> missions;
        std::unique_ptr<Library> library;
        std::unique_ptr<BuiltinModules> builtins;
    };
    /// The terrain rasters and the block cache they read through.
    struct Terrain {
        std::unique_ptr<BlockCache> blocks;
        std::shared_ptr<const DemSource> dem;
        std::shared_ptr<const LandCoverSource> landcover;
    };
    /// Calculation results: the memory tier and the database table behind it.
    struct ResultStore {
        std::unique_ptr<PgResultTable> table;
        std::unique_ptr<ResultCache> cache;
    };
    /// The forecast source (absent without weather.baseUrl) and the service over it.
    struct Forecast {
        std::unique_ptr<OpenMeteoSource> source;
        std::unique_ptr<WeatherService> service;
    };
    /// Signed modules: the host they call back into, the trusted keys, the runner and the registry.
    struct Modules {
        std::unique_ptr<ServerHostServices> host;
        std::unique_ptr<rfmod::TrustStore> trust;
        std::unique_ptr<ModuleRunner> runner;
        std::unique_ptr<rfmod::ModuleRegistry> registry;
    };

    // Declared in dependency order: members are destroyed in reverse, users before what they use.
    Config config_;
    Logger log_;
    SystemClock clock_;
    Stores store_;
    Terrain terrain_;
    ResultStore resultStore_;
    Forecast forecast_;
    Modules modules_;
    std::unique_ptr<LinkService> links_;
    std::unique_ptr<Calculator> calc_;
    std::unique_ptr<JobManager> jobs_;
    std::unique_ptr<RateLimiter> limiter_;
    HttpMetrics httpMetrics_;  ///< before http_: its request threads report here until they stop
    double startTimeSeconds_ = 0.0;
    std::unique_ptr<httplib::Server> http_;
    BundleGrants grants_;  ///< signs the UI bundle URLs of alpha modules
    std::unique_ptr<DatabaseProbe> dbProbe_;
    std::unique_ptr<OpsServer> ops_;  ///< reads the services above for /readyz and /metrics: destroyed first
    std::unique_ptr<GracefulStop> stopper_;
};

}  // namespace rfmap
