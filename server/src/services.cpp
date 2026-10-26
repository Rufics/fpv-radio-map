#include "services.hpp"

#include "migrations.hpp"
#include "tiles_manifest.hpp"

#include <format>
#include <thread>

namespace rfmap {

namespace {

/// Inline schema for POST /calc/path; refers to the mission schema for points and link setups.
json calcPathSchema() {
    const std::string m = "https://rfmap.local/schemas/mission.schema.json#/$defs/";
    return {{"$schema", "http://json-schema.org/draft-07/schema#"},
            {"type", "object"},
            {"required", {"from", "to"}},
            {"properties",
             {{"from", {{"$ref", m + "point"}}},
              {"to", {{"$ref", m + "point"}}},
              {"kind", {{"enum", {"control", "video"}}}},
              {"link", {{"$ref", m + "linkSetup"}}},
              {"links",
               {{"type", "object"},
                {"additionalProperties", false},
                {"properties", {{"control", {{"$ref", m + "linkSetup"}}}, {"video", {{"$ref", m + "linkSetup"}}}}}}},
              {"evaluate", {{"enum", {"both", "control", "video"}}}},
              {"timeUtc", {{"type", "string"}, {"format", "date-time"}}},
              {"modules", {{"type", "object"}, {"additionalProperties", {{"type", "object"}}}}}}},
            {"oneOf", {{{"required", {"kind", "link"}}}, {{"required", {"links"}}}}}};
}

json linkCreateSchema() {
    return {{"type", "object"},
            {"required", {"role", "pin"}},
            {"properties",
             {{"role", {{"enum", {"edit", "view"}}}},
              {"pin", {{"type", "string"}, {"pattern", "^[0-9]{6}$"}}},
              {"label", {{"type", "string"}, {"maxLength", 120}}},
              {"expiresAt", {{"type", "string"}, {"format", "date-time"}}}}}};
}

/// Refuses a start the configuration does not allow; warns about what it allows but should not.
void checkStartup(const Config& config, Logger& log, bool ownerKeyIsDev) {
    if (ownerKeyIsDev && !config.modules.allowDevOwnerKey) {
        throw ConfigError("this build carries the PUBLIC development owner key (RFMAP_OWNER_KEY_IS_DEV=1): anyone could "
                          "sign modules it runs in-process. Rebuild with -DRFMAP_OWNER_PUBKEY, or set "
                          "modules.allowDevOwnerKey for a development server");
    }
    if (ownerKeyIsDev) log.warn("startup", "built with the PUBLIC development owner key; modules.allowDevOwnerKey is set");
    if (config.adminToken.empty()) {
        log.warn("startup", "admin.token is not set: the admin API is disabled");
    } else if (config.adminToken.size() < Config::kMinAdminTokenLength) {
        throw ConfigError(std::format("admin.token must be at least {} characters", Config::kMinAdminTokenLength));
    }
}

/// The settings sync of `s`, with a topic for each copy of the admin settings it keeps, started.
std::unique_ptr<SettingsSync> startSettingsSync(const Services& s) {
    const Config& config = s.config();
    const Config::Database& db = config.database;
    auto sync = std::make_unique<SettingsSync>(
        s.db(),
        SettingsSync::Options{db.listenUrl.empty() ? db.url : db.listenUrl, config.settingsSync.listen,
                              std::chrono::seconds(config.settingsSync.pollSeconds)},
        s.log());
    using Source = SettingsSync::Source;
    sync->add(SettingsTopic::kBuiltins, Source::Database, [&s] { s.builtins().reload(); });
    sync->add(SettingsTopic::kCacheLimits, Source::Database, [&s] { s.caches().loadStoredLimits(); });
    sync->add(SettingsTopic::kModules, Source::Files, [&s] { s.reloadModules(); });
    sync->start();
    return sync;
}

}  // namespace

Services::Services(const Config& config, Logger& log, const std::string& ownerKeyText, bool ownerKeyIsDev,
                   const std::filesystem::path& sqlDir)
    : config_(config), log_(log), forecast_(config.weather, log, clock_) {
    Secrets::init();
    checkStartup(config_, log_, ownerKeyIsDev);
    openDatabase(sqlDir);
    buildStores();
    openTerrain();
    buildCaches();
    buildModules(ownerKeyText);
    jobs_ = std::make_unique<JobManager>(*calc_, config_.coverage.maxJobs, config_.coverage.threads,
                                         config_.coverage.keepMinutes, log_);
    limiter_ = std::make_unique<RateLimiter>(config_.rateLimits, clock_);
    settings_ = startSettingsSync(*this);
}

Services::~Services() {
    settings_.reset();  // its thread reloads the modules: stop it before they go
    jobs_.reset();  // running calculations hold module instances: finish them before the registry goes
    modules_.registry.reset();
}

void Services::openDatabase(const std::filesystem::path& sqlDir) {
    store_.db = std::make_unique<Database>(config_.database.url, config_.database.poolSize);
    // A breaking step, or a database newer than this server, stops the start: see migrations.hpp.
    const MigrationCatalog migrations(sqlDir);
    store_.db->with([this, &migrations](pqxx::connection& c) {
        return SchemaMigrator(migrations, {config_.modules.root, config_.modules.trustStore},
                              [this](const std::string& line) { log_.info("startup", line); })
            .migrate(c, {});
    });
}

namespace {

/// The PIN hashing gate for the config, its limits logged.
std::unique_ptr<PinGate> makePinGate(const Config& config, Logger& log) {
    auto gate = std::make_unique<PinGate>(
        PinGate::limitsFor(config.auth, config.server.threads, std::thread::hardware_concurrency()));
    const PinGate::Limits& l = gate->limits();
    log.info("startup",
             std::format("PIN checks: {} at once, {} waiting up to {} ms", l.concurrency, l.queue, l.wait.count()));
    if (l.concurrency + l.queue >= config.server.threads) {
        log.warn("startup", "auth.pinConcurrency + auth.pinQueue leave no server thread free during a PIN storm");
    }
    return gate;
}

/// Opens a raster over `blocks`; `name` falls back to the tiles manifest's source, then the file name.
std::shared_ptr<GdalRaster> openRaster(const std::filesystem::path& file, BlockCache& blocks, std::string& name) {
    auto raster = std::make_shared<GdalRaster>(file, blocks);
    if (name.empty()) name = TilesManifest(file).sourceName();
    if (name.empty()) name = file.filename().string();
    return raster;
}

rfmod::RegistryConfig registryConfig(const Config::Modules& modules) {
    rfmod::RegistryConfig rc;
    rc.root = modules.root;
    rc.workerExecutable = modules.worker;
    rc.hostVersion = rfmod::Version::parse(RFMAP_HOST_VERSION);
    rc.isolateAll = modules.isolateAll;
    rc.policy.requireSandbox = modules.requireSandbox;
    rc.policy.maxMemoryMb = modules.maxMemoryMb;
    rc.policy.maxTimeoutMs = modules.maxTimeoutMs;
    return rc;
}

}  // namespace

void Services::buildStores() {
    store_.schemas = std::make_unique<SchemaRegistry>(config_.schemasDir);
    store_.schemas->addInline("calc-path", calcPathSchema());
    store_.schemas->addInline("link-create", linkCreateSchema());
    store_.authStore = std::make_unique<PgAuthStore>(*store_.db);
    store_.pins = std::make_unique<PinHasher>(PinHasher::costFromName(config_.auth.argon2Cost));
    store_.pinGate = makePinGate(config_, log_);
    store_.auth = std::make_unique<AuthService>(
        *store_.authStore, *store_.pins, *store_.pinGate, clock_,
        AuthService::Settings{config_.auth.pinMaxAttempts, std::chrono::minutes(config_.auth.pinLockoutMinutes),
                              std::chrono::hours(config_.auth.sessionTtlHours)});
    store_.missions = std::make_unique<MissionStore>(*store_.db);
    store_.flightLogs = std::make_unique<FlightLogStore>(*store_.db);
    store_.library = std::make_unique<Library>(*store_.db, *store_.schemas);
    store_.builtins = std::make_unique<BuiltinModules>(store_.db.get(), *store_.schemas, RFMAP_HOST_VERSION);
    if (store_.builtins->enabled(BuiltinModules::kRadioProfiles)) {
        for (const auto& line : store_.library->seed(config_.seedDir)) log_.info("library", line);
    }
}

void Services::openTerrain() {
    terrain_.blocks = std::make_unique<BlockCache>(config_.cache.demBlocksMb << 20, [](const RasterBlock& b) {
        return b.values.size() * sizeof(float) + sizeof(RasterBlock);
    });
    const Config::Data& data = config_.data;
    if (!data.dem.empty()) {
        std::string name = data.demName;
        auto raster = openRaster(data.dem, *terrain_.blocks, name);
        log_.info("terrain", std::format("DEM {} ({}, step {} m): {}", data.dem.string(), raster->dataType(),
                                         std::to_string(raster->nativeResolution().value()), name));
        terrain_.dem = std::make_shared<DemSource>(raster, name, data.demStatedErrorM, data.demStatedErrorSource);
        if (!data.demStatedErrorM.has_value()) {
            log_.warn("terrain", "data.demStatedErrorM is not set: results carry no terrain error");
        }
    } else {
        log_.warn("terrain", "no DEM configured (data.dem): calculations answer 503");
    }
    // Land cover is optional (clutter is opt-in): a missing file is reported, not fatal. A
    // configured DEM that cannot be opened stops the start, since nothing can be calculated.
    if (data.landcover.empty()) return;
    if (!std::filesystem::exists(data.landcover)) {
        log_.warn("terrain", "land cover " + data.landcover.string() + " not found: clutter is not applied and results say so");
        return;
    }
    std::string name = data.landcoverName;
    auto raster = openRaster(data.landcover, *terrain_.blocks, name);
    terrain_.landcover = std::make_shared<LandCoverSource>(raster, name);
}

void Services::buildCaches() {
    resultStore_.table = std::make_unique<PgResultTable>(*store_.db);
    resultStore_.cache = std::make_unique<ResultCache>(
        ResultCache::Limits{config_.cache.resultMemoryMb << 20, config_.cache.resultTableRows,
                            config_.cache.resultTableMb << 20},
        resultStore_.table.get());
    resultStore_.admin = std::make_unique<CacheAdmin>(*store_.db, *terrain_.blocks, *resultStore_.cache);
    resultStore_.admin->loadStoredLimits();
}

Services::Forecast::Forecast(const Config::Weather& weather, Logger& log, const IClock& clock) {
    if (!weather.baseUrl.empty()) source = std::make_unique<OpenMeteoSource>(weather);
    else log.warn("weather", "weather.baseUrl is not set: no forecasts, k = 4/3 everywhere");
    service = std::make_unique<WeatherService>(weather, source.get(), clock);
}

void Services::buildModules(const std::string& ownerKeyText) {
    // The host answers module calls only inside a request's InvocationScope, so it needs no
    // service of its own: the registry can load modules before the calculations exist.
    modules_.host = std::make_unique<ServerHostServices>(log_);
    modules_.trust = std::make_unique<rfmod::TrustStore>(ownerKeyText, config_.modules.trustStore);
    moduleGate_->attach(config_.modules.root);
    {
        // Loading restores the files of a module that differ from its signed package, so a backup
        // must not read them meanwhile.
        const ModuleGate::Change gate(*moduleGate_);
        modules_.registry =
            std::make_unique<rfmod::ModuleRegistry>(registryConfig(config_.modules), *modules_.trust, *modules_.host);
        modules_.registry->reload();
    }
    for (const auto& n : modules_.registry->notices()) log_.warn("modules", n);
    modules_.runner = std::make_unique<ModuleRunner>(*modules_.registry);
    modules_.catalog = std::make_unique<ModuleCatalog>(*store_.builtins, *modules_.registry, modules_.grants);
    links_ = std::make_unique<LinkService>(terrain_.dem, terrain_.landcover, *store_.library, *store_.builtins,
                                           modules_.runner.get(), RFMAP_HOST_VERSION);
    calc_ = std::make_unique<Calculator>(*links_, *forecast_.service, *store_.builtins, *modules_.runner,
                                         *resultStore_.cache, *store_.schemas);
    flightLogs_ = std::make_unique<FlightLogService>(*modules_.runner, *store_.schemas, store_.flightLogs.get(),
                                                     *store_.builtins,
                                                     FlightLogService::Limits{config_.server.maxUploadBytes, 200});
}

void Services::reloadModules() const {
    const ModuleGate::Change gate(*moduleGate_);
    modules_.trust->reload();
    modules_.registry->reload();
    for (const auto& n : modules_.registry->notices()) log_.warn("modules", n);
}

}  // namespace rfmap
