#include "services.hpp"

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

}  // namespace

Services::Services(const Config& config, Logger& log, const std::string& ownerKeyText, bool ownerKeyIsDev,
                   const std::filesystem::path& sqlDir)
    : config_(config), log_(log) {
    Secrets::init();
    checkStartup(ownerKeyIsDev);
    openDatabase(sqlDir);
    buildStores();
    openTerrain();
    buildCaches();
    buildForecast();
    buildModules(ownerKeyText);
    jobs_ = std::make_unique<JobManager>(*calc_, config_.coverage.maxJobs, config_.coverage.threads,
                                         config_.coverage.keepMinutes, log_);
    limiter_ = std::make_unique<RateLimiter>(config_.rateLimits, clock_);
}

Services::~Services() {
    jobs_.reset();  // running calculations hold module instances: finish them before the registry goes
    modules_.registry.reset();
}

void Services::checkStartup(bool ownerKeyIsDev) const {
    if (ownerKeyIsDev && !config_.modules.allowDevOwnerKey) {
        throw ConfigError("this build carries the PUBLIC development owner key (RFMAP_OWNER_KEY_IS_DEV=1): anyone could "
                          "sign modules it runs in-process. Rebuild with -DRFMAP_OWNER_PUBKEY, or set "
                          "modules.allowDevOwnerKey for a development server");
    }
    if (ownerKeyIsDev) log_.warn("startup", "built with the PUBLIC development owner key; modules.allowDevOwnerKey is set");
    if (config_.adminToken.empty()) {
        log_.warn("startup", "admin.token is not set: the admin API is disabled");
    } else if (config_.adminToken.size() < Config::kMinAdminTokenLength) {
        throw ConfigError(std::format("admin.token must be at least {} characters", Config::kMinAdminTokenLength));
    }
}

void Services::openDatabase(const std::filesystem::path& sqlDir) {
    store_.db = std::make_unique<Database>(config_.database.url, config_.database.poolSize);
    for (const auto& m : store_.db->migrate(sqlDir)) log_.info("startup", "applied migration " + m);
}

std::unique_ptr<PinGate> Services::makePinGate() const {
    auto gate = std::make_unique<PinGate>(
        PinGate::limitsFor(config_.auth, config_.server.threads, std::thread::hardware_concurrency()));
    const PinGate::Limits& l = gate->limits();
    log_.info("startup",
              std::format("PIN checks: {} at once, {} waiting up to {} ms", l.concurrency, l.queue, l.wait.count()));
    if (l.concurrency + l.queue >= config_.server.threads) {
        log_.warn("startup", "auth.pinConcurrency + auth.pinQueue leave no server thread free during a PIN storm");
    }
    return gate;
}

void Services::buildStores() {
    store_.schemas = std::make_unique<SchemaRegistry>(config_.schemasDir);
    store_.schemas->addInline("calc-path", calcPathSchema());
    store_.schemas->addInline("link-create", linkCreateSchema());
    store_.authStore = std::make_unique<PgAuthStore>(*store_.db);
    store_.pins = std::make_unique<PinHasher>(PinHasher::costFromName(config_.auth.argon2Cost));
    store_.pinGate = makePinGate();
    store_.auth = std::make_unique<AuthService>(
        *store_.authStore, *store_.pins, *store_.pinGate, clock_,
        AuthService::Settings{config_.auth.pinMaxAttempts, std::chrono::minutes(config_.auth.pinLockoutMinutes),
                              std::chrono::hours(config_.auth.sessionTtlHours)});
    store_.missions = std::make_unique<MissionStore>(*store_.db);
    store_.library = std::make_unique<Library>(*store_.db, *store_.schemas);
    store_.builtins = std::make_unique<BuiltinModules>(store_.db.get(), *store_.schemas, RFMAP_HOST_VERSION);
    if (store_.builtins->enabled(BuiltinModules::kRadioProfiles)) {
        for (const auto& line : store_.library->seed(config_.seedDir)) log_.info("library", line);
    }
}

std::shared_ptr<GdalRaster> Services::openRaster(const std::filesystem::path& file, std::string& name) const {
    auto raster = std::make_shared<GdalRaster>(file, *terrain_.blocks);
    if (name.empty()) name = TilesManifest(file).sourceName();
    if (name.empty()) name = file.filename().string();
    return raster;
}

void Services::openTerrain() {
    terrain_.blocks = std::make_unique<BlockCache>(config_.cache.demBlocksMb << 20, [](const RasterBlock& b) {
        return b.values.size() * sizeof(float) + sizeof(RasterBlock);
    });
    const Config::Data& data = config_.data;
    if (!data.dem.empty()) {
        std::string name = data.demName;
        auto raster = openRaster(data.dem, name);
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
    auto raster = openRaster(data.landcover, name);
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

void Services::buildForecast() {
    if (!config_.weather.baseUrl.empty()) forecast_.source = std::make_unique<OpenMeteoSource>(config_.weather);
    else log_.warn("weather", "weather.baseUrl is not set: no forecasts, k = 4/3 everywhere");
    forecast_.service = std::make_unique<WeatherService>(config_.weather, forecast_.source.get(), clock_);
}

rfmod::RegistryConfig Services::registryConfig() const {
    rfmod::RegistryConfig rc;
    rc.root = config_.modules.root;
    rc.workerExecutable = config_.modules.worker;
    rc.hostVersion = rfmod::Version::parse(RFMAP_HOST_VERSION);
    rc.isolateAll = config_.modules.isolateAll;
    rc.policy.requireSandbox = config_.modules.requireSandbox;
    rc.policy.maxMemoryMb = config_.modules.maxMemoryMb;
    rc.policy.maxTimeoutMs = config_.modules.maxTimeoutMs;
    return rc;
}

void Services::buildModules(const std::string& ownerKeyText) {
    // The host answers module calls only inside a request's InvocationScope, so it needs no
    // service of its own: the registry can load modules before the calculations exist.
    modules_.host = std::make_unique<ServerHostServices>(log_);
    modules_.trust = std::make_unique<rfmod::TrustStore>(ownerKeyText, config_.modules.trustStore);
    modules_.registry = std::make_unique<rfmod::ModuleRegistry>(registryConfig(), *modules_.trust, *modules_.host);
    modules_.registry->reload();
    for (const auto& n : modules_.registry->notices()) log_.warn("modules", n);
    modules_.runner = std::make_unique<ModuleRunner>(*modules_.registry);
    modules_.catalog = std::make_unique<ModuleCatalog>(*store_.builtins, *modules_.registry, modules_.grants);
    links_ = std::make_unique<LinkService>(terrain_.dem, terrain_.landcover, *store_.library, *store_.builtins,
                                           modules_.runner.get(), RFMAP_HOST_VERSION);
    calc_ = std::make_unique<Calculator>(*links_, *forecast_.service, *store_.builtins, *modules_.runner,
                                         *resultStore_.cache, *store_.schemas);
}

void Services::reloadModules() const {
    modules_.trust->reload();
    modules_.registry->reload();
    for (const auto& n : modules_.registry->notices()) log_.warn("modules", n);
}

}  // namespace rfmap
