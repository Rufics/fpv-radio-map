#pragma once

#include "auth.hpp"
#include "builtins.hpp"
#include "calculator.hpp"
#include "config.hpp"
#include "database.hpp"
#include "host_services.hpp"
#include "jobs.hpp"
#include "library.hpp"
#include "logger.hpp"
#include "missions.hpp"
#include "module_runner.hpp"
#include "pg_auth_store.hpp"
#include "rate_limiter.hpp"
#include "raster.hpp"
#include "result_cache.hpp"
#include "schemas.hpp"
#include "weather.hpp"

#include <rfmod/registry.hpp>
#include <rfmod/trust.hpp>

#include <memory>
#include <string>

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
    Application(Config config, std::string ownerKeyText, bool ownerKeyIsDev, std::filesystem::path sqlDir);
    ~Application();

    /// Binds and serves until stop(). Returns false if the port could not be bound.
    bool listen();
    /// Binds to an ephemeral port on the configured address; returns it (for tests).
    int bindAny();
    void serveBound();
    void stop();

    [[nodiscard]] Logger& log() { return log_; }
    [[nodiscard]] const Config& config() const { return config_; }

private:
    /// One endpoint; routes() registers each with its path and rate class.
    using Handler = void (Application::*)(const httplib::Request&, httplib::Response&);

    void routes();
    /// Runs `handler` under the rate limit of `rateClass` and answers its errors as JSON.
    void dispatch(const std::string& rateClass, Handler handler, const httplib::Request& req, httplib::Response& res);
    void loadCacheSettings();
    json cacheTiers();
    json moduleList(bool admin);
    json mapConfig() const;

    // request helpers
    [[nodiscard]] std::string client(const httplib::Request& req) const;
    [[nodiscard]] json body(const httplib::Request& req) const;
    [[nodiscard]] Principal session(const httplib::Request& req);
    void requireAdmin(const httplib::Request& req);
    [[nodiscard]] bool isAdmin(const httplib::Request& req) const;
    [[nodiscard]] std::string bundleGrant(const std::string& moduleId, TimePoint until) const;
    [[nodiscard]] bool grantValid(const std::string& moduleId, const std::string& grant) const;
    /// The library kind of a path, checked: HttpError 404 for an unknown or disabled kind.
    [[nodiscard]] std::string libraryKind(const std::string& kind) const;
    [[nodiscard]] json trustedKeys() const;

    // route handlers, in the order routes() registers them
    void getHealth(const httplib::Request& req, httplib::Response& res);
    void postMission(const httplib::Request& req, httplib::Response& res);
    void getMission(const httplib::Request& req, httplib::Response& res);
    void putMission(const httplib::Request& req, httplib::Response& res);
    void getLinks(const httplib::Request& req, httplib::Response& res);
    void postLink(const httplib::Request& req, httplib::Response& res);
    void deleteLink(const httplib::Request& req, httplib::Response& res);
    void getLink(const httplib::Request& req, httplib::Response& res);
    void postLinkOpen(const httplib::Request& req, httplib::Response& res);
    void postCalcPath(const httplib::Request& req, httplib::Response& res);
    void postCalcRoute(const httplib::Request& req, httplib::Response& res);
    void postCalcCoverage(const httplib::Request& req, httplib::Response& res);
    void getJob(const httplib::Request& req, httplib::Response& res);
    void deleteJob(const httplib::Request& req, httplib::Response& res);
    void getJobGrid(const httplib::Request& req, httplib::Response& res);
    void getElevationProfile(const httplib::Request& req, httplib::Response& res);
    void getLibraryList(const httplib::Request& req, httplib::Response& res);
    void getLibraryItem(const httplib::Request& req, httplib::Response& res);
    void getLibraryHistory(const httplib::Request& req, httplib::Response& res);
    void postLibraryItem(const httplib::Request& req, httplib::Response& res);
    void putLibraryItem(const httplib::Request& req, httplib::Response& res);
    void getWeatherPoint(const httplib::Request& req, httplib::Response& res);
    void getWeatherHorizon(const httplib::Request& req, httplib::Response& res);
    void getMapConfig(const httplib::Request& req, httplib::Response& res);
    void getModules(const httplib::Request& req, httplib::Response& res);
    void getModuleFile(const httplib::Request& req, httplib::Response& res);
    void postAdminModule(const httplib::Request& req, httplib::Response& res);
    void putAdminModule(const httplib::Request& req, httplib::Response& res);
    void deleteAdminModule(const httplib::Request& req, httplib::Response& res);
    void getAdminNotices(const httplib::Request& req, httplib::Response& res);
    void getAdminKeys(const httplib::Request& req, httplib::Response& res);
    void postAdminKey(const httplib::Request& req, httplib::Response& res);
    void deleteAdminKey(const httplib::Request& req, httplib::Response& res);
    void getAdminCache(const httplib::Request& req, httplib::Response& res);
    void putAdminCache(const httplib::Request& req, httplib::Response& res);
    void deleteAdminCache(const httplib::Request& req, httplib::Response& res);

    Config config_;
    Logger log_;
    SystemClock clock_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<SchemaRegistry> schemas_;
    std::unique_ptr<PgAuthStore> authStore_;
    std::unique_ptr<PinHasher> pins_;
    std::unique_ptr<AuthService> auth_;
    std::unique_ptr<MissionStore> missions_;
    std::unique_ptr<Library> library_;
    std::unique_ptr<BuiltinModules> builtins_;
    std::unique_ptr<BlockCache> blocks_;
    std::shared_ptr<const DemSource> dem_;
    std::shared_ptr<const LandCoverSource> landcover_;
    std::unique_ptr<PgResultTable> resultTable_;
    std::unique_ptr<ResultCache> results_;
    std::unique_ptr<OpenMeteoSource> forecastSource_;
    std::unique_ptr<WeatherService> weather_;
    std::unique_ptr<ServerHostServices> host_;
    std::unique_ptr<rfmod::TrustStore> trust_;
    std::unique_ptr<ModuleRunner> runner_;
    std::unique_ptr<rfmod::ModuleRegistry> registry_;
    std::unique_ptr<LinkService> links_;
    std::unique_ptr<Calculator> calc_;
    std::unique_ptr<JobManager> jobs_;
    std::unique_ptr<RateLimiter> limiter_;
    std::unique_ptr<httplib::Server> http_;
    std::string grantSecret_;
};

}  // namespace rfmap
