#pragma once

#include "auth.hpp"
#include "builtins.hpp"
#include "cache_admin.hpp"
#include "calculator.hpp"
#include "config.hpp"
#include "database.hpp"
#include "file_lock.hpp"
#include "flight_logs.hpp"
#include "host_services.hpp"
#include "jobs.hpp"
#include "library.hpp"
#include "logger.hpp"
#include "missions.hpp"
#include "module_catalog.hpp"
#include "module_runner.hpp"
#include "pg_auth_store.hpp"
#include "rate_limiter.hpp"
#include "raster.hpp"
#include "result_cache.hpp"
#include "schemas.hpp"
#include "settings_sync.hpp"
#include "weather.hpp"

#include <rfmod/registry.hpp>
#include <rfmod/trust.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace rfmap {

/// Everything the HTTP handlers work with, built once in dependency order: the database and
/// the stores on it, the terrain, the caches, the forecast, the modules, the calculations.
/// Members are destroyed in reverse, users before what they use.
class Services {
public:
    /// `ownerKeyText` is the built-in owner key (RFMAP_OWNER_PUBKEY_TEXT); `ownerKeyIsDev` says
    /// the build used the public development key, which only `modules.allowDevOwnerKey` permits.
    Services(const Config& config, Logger& log, const std::string& ownerKeyText, bool ownerKeyIsDev,
             const std::filesystem::path& sqlDir);
    ~Services();
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] Logger& log() const { return log_; }
    [[nodiscard]] const IClock& clock() const { return clock_; }
    [[nodiscard]] Database& db() const { return *store_.db; }
    [[nodiscard]] const SchemaRegistry& schemas() const { return *store_.schemas; }
    [[nodiscard]] AuthService& auth() const { return *store_.auth; }
    [[nodiscard]] MissionStore& missions() const { return *store_.missions; }
    [[nodiscard]] Library& library() const { return *store_.library; }
    [[nodiscard]] BuiltinModules& builtins() const { return *store_.builtins; }
    [[nodiscard]] const DemSource* dem() const { return terrain_.dem.get(); }
    [[nodiscard]] const LandCoverSource* landcover() const { return terrain_.landcover.get(); }
    [[nodiscard]] BlockCache& blocks() const { return *terrain_.blocks; }
    [[nodiscard]] ResultCache& results() const { return *resultStore_.cache; }
    [[nodiscard]] const CacheAdmin& caches() const { return *resultStore_.admin; }
    [[nodiscard]] WeatherService& weather() const { return *forecast_.service; }
    [[nodiscard]] rfmod::TrustStore& trust() const { return *modules_.trust; }
    [[nodiscard]] rfmod::ModuleRegistry& registry() const { return *modules_.registry; }
    [[nodiscard]] const ModuleCatalog& catalog() const { return *modules_.catalog; }
    [[nodiscard]] LinkService& links() const { return *links_; }
    [[nodiscard]] Calculator& calc() const { return *calc_; }
    [[nodiscard]] FlightLogService& flightLogs() const { return *flightLogs_; }
    [[nodiscard]] JobManager& jobs() const { return *jobs_; }
    [[nodiscard]] RateLimiter& limiter() const { return *limiter_; }
    /// Keeps the admin settings of this process in step with the other processes.
    [[nodiscard]] SettingsSync& settings() const { return *settings_; }

    /// Re-reads the trust store and the module root: what the rfmod CLI installed or removed.
    void reloadModules() const;
    /// Held around every change the server makes to the module files, see ModuleGate.
    [[nodiscard]] ModuleGate& moduleGate() const { return *moduleGate_; }

private:
    // Also runs reloads from parallel admin requests in turn.
    std::unique_ptr<ModuleGate> moduleGate_ = std::make_unique<ModuleGate>();
    void checkStartup(bool ownerKeyIsDev) const;
    void openDatabase(const std::filesystem::path& sqlDir);
    void buildStores();
    void openTerrain();
    void buildCaches();
    void buildModules(const std::string& ownerKeyText);
    void startSettingsSync();

    /// Persistent state: the database and the stores and services built on it.
    struct Stores {
        std::unique_ptr<Database> db;
        std::unique_ptr<SchemaRegistry> schemas;
        std::unique_ptr<PgAuthStore> authStore;
        std::unique_ptr<PinHasher> pins;
        std::unique_ptr<PinGate> pinGate;
        std::unique_ptr<AuthService> auth;
        std::unique_ptr<MissionStore> missions;
        std::unique_ptr<FlightLogStore> flightLogs;
        std::unique_ptr<Library> library;
        std::unique_ptr<BuiltinModules> builtins;
    };
    /// The terrain rasters and the block cache they read through.
    struct Terrain {
        std::unique_ptr<BlockCache> blocks;
        std::shared_ptr<const DemSource> dem;
        std::shared_ptr<const LandCoverSource> landcover;
    };
    /// Calculation results: the memory tier, the database table behind it, and their admin.
    struct ResultStore {
        std::unique_ptr<PgResultTable> table;
        std::unique_ptr<ResultCache> cache;
        std::unique_ptr<CacheAdmin> admin;
    };
    /// The forecast source (absent without weather.baseUrl) and the service over it.
    struct Forecast {
        Forecast(const Config::Weather& weather, Logger& log, const IClock& clock);
        std::unique_ptr<OpenMeteoSource> source;
        std::unique_ptr<WeatherService> service;
    };
    /// Signed modules: the host they call back into, the trusted keys, the registry over the
    /// module root, the runner the calculations use and the catalog the API shows.
    struct Modules {
        std::unique_ptr<ServerHostServices> host;
        std::unique_ptr<rfmod::TrustStore> trust;
        std::unique_ptr<rfmod::ModuleRegistry> registry;
        std::unique_ptr<ModuleRunner> runner;
        BundleGrants grants;  ///< signs the UI bundle URLs of alpha modules
        std::unique_ptr<ModuleCatalog> catalog;
    };

    const Config& config_;
    Logger& log_;
    SystemClock clock_;
    Stores store_;
    Terrain terrain_;
    ResultStore resultStore_;
    Forecast forecast_;
    Modules modules_;
    std::unique_ptr<LinkService> links_;
    std::unique_ptr<Calculator> calc_;
    std::unique_ptr<FlightLogService> flightLogs_;
    std::unique_ptr<JobManager> jobs_;
    std::unique_ptr<RateLimiter> limiter_;
    std::unique_ptr<SettingsSync> settings_;  ///< its reloads use the members above
};

}  // namespace rfmap
