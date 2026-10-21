#include "app.hpp"

#include <httplib.h>

#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <thread>

namespace rfmap {

namespace {

using Req = httplib::Request;
using Res = httplib::Response;

void sendJson(Res& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

std::string header(const Req& req, const char* name) { return req.has_header(name) ? req.get_header_value(name) : ""; }

std::string bearer(const Req& req) {
    if (const std::string h = header(req, "Authorization"); h.starts_with("Bearer ")) return h.substr(7);
    return {};
}

/// httplib's request thread pool, counting the requests that wait for a thread and those served.
class CountingTaskQueue final : public httplib::TaskQueue {
public:
    CountingTaskQueue(std::size_t threads, HttpMetrics& metrics) : pool_(threads), metrics_(metrics) {}

    // Counted as queued before the pool can start it; a request the pool turns away (it does
    // so only while shutting down) leaves the queue again.
    bool enqueue(std::function<void()> fn) override {
        metrics_.enqueued();
        const bool accepted = pool_.enqueue([this, fn = std::move(fn)] {
            metrics_.started();
            const Served served{metrics_};
            fn();
        });
        if (!accepted) metrics_.rejected();
        return accepted;
    }
    void shutdown() override { pool_.shutdown(); }

private:
    /// Ends the busy count also when the request handler throws.
    struct Served {
        explicit Served(HttpMetrics& m) : metrics(m) {}
        HttpMetrics& metrics;
        Served(const Served&) = delete;
        Served& operator=(const Served&) = delete;
        ~Served() { metrics.finished(); }
    };

    httplib::ThreadPool pool_;
    HttpMetrics& metrics_;
};

/// Paths carry link tokens and module ids; the token must never reach the log.
std::string redacted(const std::string& path) {
    static const std::regex link("/links/[^/]+(/open)?$");
    return std::regex_replace(path, link, "/links/***$1");
}

rfcore::GeoPoint parseLatLon(const std::string& text) {
    static const std::regex re(R"(^\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*,\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*$)");
    std::smatch m;
    if (!std::regex_match(text, m, re)) throw HttpError(400, "expected lat,lon, got '" + text + "'");
    const double lat = std::stod(m[1]);
    const double lon = std::stod(m[2]);
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) throw HttpError(400, "lat,lon out of range");
    return {lat, lon};
}

double queryNumber(const Req& req, const char* name) {
    if (!req.has_param(name)) throw HttpError(400, std::string("query parameter ") + name + " is required");
    try {
        std::size_t used = 0;
        const std::string v = req.get_param_value(name);
        const double d = std::stod(v, &used);
        if (used != v.size() || !std::isfinite(d)) throw std::invalid_argument("x");
        return d;
    } catch (const std::exception&) {
        throw HttpError(400, std::string("query parameter ") + name + " must be a number");
    }
}

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

/// Drops the map layers the tiles-builder manifest next to `dem` does not list, so the client
/// shows no satellite view it cannot load. Without a manifest every layer stays.
void dropUnbuiltLayers(json& map, const std::filesystem::path& dem) {
    const auto files = manifestFiles(dem);
    if (!files) return;
    for (const char* layer : {"basemap", "satellite", "hillshade", "contours"}) {
        if (!files->contains(std::string(layer) + ".pmtiles")) map.erase(layer);
    }
}

/// The revision in an If-Match header ("3", W/"3"); HttpError 428 without one.
int ifMatchRevision(const Req& req) {
    std::string match = header(req, "If-Match");
    std::erase_if(match, [](char c) { return c == '"' || c == 'W' || c == '/'; });
    try {
        return std::stoi(match);
    } catch (const std::exception&) {
        throw HttpError(428, "If-Match with the revision you edited is required");
    }
}

/// Content type of a module's public file, by extension.
const char* moduleFileType(std::string_view ext) {
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".css") return "text/css";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    return "application/octet-stream";
}

/// Answers an HttpError as JSON {message, ...body}, with Retry-After on 429 and 503.
void sendHttpError(Res& res, const HttpError& e) {
    json b = e.body();
    b["message"] = e.what();
    if ((e.status() == 429 || e.status() == 503) && b.contains("retryAfterS")) {
        res.set_header("Retry-After", std::to_string(b["retryAfterS"].get<long long>()));
    }
    sendJson(res, e.status(), b);
}

}  // namespace

Application::Application(Config config, const std::string& ownerKeyText, bool ownerKeyIsDev, const std::filesystem::path& sqlDir)
    : config_(std::move(config)) {
    Secrets::init();
    if (ownerKeyIsDev) {
        log_.warn("startup", "built with the PUBLIC development owner key (RFMAP_OWNER_KEY_IS_DEV=1): anyone can sign "
                             "modules this server runs in-process. Rebuild with -DRFMAP_OWNER_PUBKEY for production.");
    }
    if (config_.adminToken.empty()) log_.warn("startup", "admin.token is not set: the admin API is disabled");
    else if (config_.adminToken.size() < 24) log_.warn("startup", "admin.token is shorter than 24 characters");

    store_.db = std::make_unique<Database>(config_.database.url, config_.database.poolSize);
    for (const auto& m : store_.db->migrate(sqlDir)) log_.info("startup", "applied migration " + m);

    store_.schemas = std::make_unique<SchemaRegistry>(config_.schemasDir);
    store_.schemas->addInline("calc-path", calcPathSchema());
    store_.schemas->addInline("link-create", {{"type", "object"},
                                        {"required", {"role", "pin"}},
                                        {"properties",
                                         {{"role", {{"enum", {"edit", "view"}}}},
                                          {"pin", {{"type", "string"}, {"pattern", "^[0-9]{6}$"}}},
                                          {"label", {{"type", "string"}, {"maxLength", 120}}},
                                          {"expiresAt", {{"type", "string"}, {"format", "date-time"}}}}}});

    store_.authStore = std::make_unique<PgAuthStore>(*store_.db);
    store_.pins = std::make_unique<PinHasher>(PinHasher::costFromName(config_.auth.argon2Cost));
    store_.pinGate = makePinGate();
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

    terrain_.blocks = std::make_unique<BlockCache>(config_.cache.demBlocksMb << 20, [](const RasterBlock& b) {
        return b.values.size() * sizeof(float) + sizeof(RasterBlock);
    });
    if (!config_.data.dem.empty()) {
        auto raster = std::make_shared<GdalRaster>(config_.data.dem, *terrain_.blocks);
        std::string name = config_.data.demName;
        if (name.empty()) name = manifestSourceName(config_.data.dem);
        if (name.empty()) name = config_.data.dem.filename().string();
        terrain_.dem =
            std::make_shared<DemSource>(raster, name, config_.data.demStatedErrorM, config_.data.demStatedErrorSource);
        log_.info("terrain", std::format("DEM {} ({}, step {} m): {}", config_.data.dem.string(), raster->dataType(),
                                         std::to_string(raster->nativeResolution().value()), name));
        if (!config_.data.demStatedErrorM.has_value()) {
            log_.warn("terrain", "data.demStatedErrorM is not set: results carry no terrain error");
        }
    } else {
        log_.warn("terrain", "no DEM configured (data.dem): calculations answer 503");
    }
    // Land cover is optional (clutter is opt-in): a missing file is reported, not fatal. A
    // configured DEM that cannot be opened stops the start, since nothing can be calculated.
    if (!config_.data.landcover.empty() && !std::filesystem::exists(config_.data.landcover)) {
        log_.warn("terrain", "land cover " + config_.data.landcover.string() +
                                 " not found: clutter is not applied and results say so");
    } else if (!config_.data.landcover.empty()) {
        auto raster = std::make_shared<GdalRaster>(config_.data.landcover, *terrain_.blocks);
        std::string name = config_.data.landcoverName;
        if (name.empty()) name = manifestSourceName(config_.data.landcover);
        if (name.empty()) name = config_.data.landcover.filename().string();
        terrain_.landcover = std::make_shared<LandCoverSource>(raster, name);
    }

    resultStore_.table = std::make_unique<PgResultTable>(*store_.db);
    resultStore_.cache = std::make_unique<ResultCache>(
        ResultCache::Limits{config_.cache.resultMemoryMb << 20, config_.cache.resultTableRows,
                            config_.cache.resultTableMb << 20},
        resultStore_.table.get());
    loadCacheSettings();

    if (!config_.weather.baseUrl.empty()) forecast_.source = std::make_unique<OpenMeteoSource>(config_.weather);
    else log_.warn("weather", "weather.baseUrl is not set: no forecasts, k = 4/3 everywhere");
    forecast_.service = std::make_unique<WeatherService>(config_.weather, forecast_.source.get(), clock_);

    // Modules can call back into the server from init, so the host must be complete before
    // the registry loads anything.
    modules_.host = std::make_unique<ServerHostServices>(nullptr, nullptr, nullptr, nullptr, log_);
    modules_.trust = std::make_unique<rfmod::TrustStore>(ownerKeyText, config_.modules.trustStore);
    modules_.runner = std::make_unique<ModuleRunner>(nullptr);
    links_ = std::make_unique<LinkService>(terrain_.dem, terrain_.landcover, *store_.library, *store_.builtins,
                                           modules_.runner.get(), RFMAP_HOST_VERSION);
    calc_ = std::make_unique<Calculator>(*links_, *forecast_.service, *store_.builtins, *modules_.runner,
                                         *resultStore_.cache, log_);
    flightLogs_ = std::make_unique<FlightLogService>(*modules_.runner, *store_.schemas, store_.flightLogs.get(),
                                                     *store_.builtins,
                                                     FlightLogService::Limits{config_.server.maxUploadBytes, 200});
    modules_.host->bind(links_.get(), store_.library.get(), forecast_.service.get(),
                        [this](const json& m) { return calc_->contextFor(m); }, store_.schemas.get());

    rfmod::RegistryConfig rc;
    rc.root = config_.modules.root;
    rc.workerExecutable = config_.modules.worker;
    rc.hostVersion = rfmod::Version::parse(RFMAP_HOST_VERSION);
    rc.isolateAll = config_.modules.isolateAll;
    modules_.registry = std::make_unique<rfmod::ModuleRegistry>(rc, *modules_.trust, *modules_.host);
    modules_.registry->reload();
    modules_.runner->setRegistry(modules_.registry.get());
    for (const auto& n : modules_.registry->notices()) log_.warn("modules", n);

    jobs_ = std::make_unique<JobManager>(*calc_, config_.coverage.maxJobs, config_.coverage.threads,
                                         config_.coverage.keepMinutes, log_);
    limiter_ = std::make_unique<RateLimiter>(config_.rateLimits, clock_);
    startTimeSeconds_ =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    http_ = std::make_unique<httplib::Server>();
    routes();
}

Application::~Application() {
    if (http_) http_->stop();
    jobs_.reset();
    modules_.registry.reset();
}

bool Application::listen() {
    log_.info("http", std::format("listening on {}:{}", config_.server.listen, config_.server.port));
    return http_->listen(config_.server.listen, config_.server.port);
}

int Application::bindAny() { return http_->bind_to_any_port(config_.server.listen); }
void Application::serveBound() { http_->listen_after_bind(); }
void Application::stop() { http_->stop(); }

void Application::loadCacheSettings() const {
    const auto stored = store_.db->tx([](pqxx::work& w) -> std::optional<json> {
        const auto r = w.exec("select value::text from settings where key = 'cache.limits'");
        if (r.empty()) return std::nullopt;
        return std::make_optional(json::parse(r[0][0].as<std::string>()));
    });
    if (!stored) return;
    if (stored->contains("demBlocksBytes")) terrain_.blocks->setLimit(stored->at("demBlocksBytes").get<std::size_t>());
    ResultCache::Limits l = resultStore_.cache->limits();
    l.memoryBytes = stored->value("resultMemoryBytes", l.memoryBytes);
    l.tableRows = stored->value("resultTableRows", l.tableRows);
    l.tableBytes = stored->value("resultTableBytes", l.tableBytes);
    resultStore_.cache->setLimits(l);
}

json Application::cacheTiers() const {
    const CacheStats dem = terrain_.blocks->stats();
    const CacheStats mem = resultStore_.cache->memoryStats();
    const auto table = resultStore_.cache->tableStats();
    const auto limits = resultStore_.cache->limits();
    const auto rate = [](std::size_t h, std::size_t m) {
        return h + m == 0 ? 0.0 : static_cast<double>(h) / static_cast<double>(h + m);
    };
    return json::array(
        {{{"id", "dem-blocks"}, {"label", "Terrain and land cover blocks (memory)"}, {"limitBytes", dem.limitBytes},
          {"usedBytes", dem.usedBytes}, {"entries", dem.entries}, {"hits", dem.hits}, {"misses", dem.misses},
          {"evictions", dem.evictions}, {"hitRate", dem.hitRate()}},
         {{"id", "results-memory"}, {"label", "Results (memory)"}, {"limitBytes", mem.limitBytes},
          {"usedBytes", mem.usedBytes}, {"entries", mem.entries}, {"hits", mem.hits}, {"misses", mem.misses},
          {"evictions", mem.evictions}, {"hitRate", mem.hitRate()}},
         {{"id", "results-table"}, {"label", "Results (database table)"}, {"limitBytes", limits.tableBytes},
          {"usedBytes", table.bytes}, {"limitRows", limits.tableRows}, {"entries", table.rows}, {"hits", table.hits},
          {"misses", table.misses}, {"hitRate", rate(table.hits, table.misses)}}});
}

std::string Application::metricsText() {
    ServerSample sample;
    sample.version = RFMAP_HOST_VERSION;
    sample.startTimeSeconds = startTimeSeconds_;
    sample.process = readProcessStats();
    sample.jobs = jobs_->stats();
    sample.pools = {{"http", httpMetrics_.pool(config_.server.threads)},
                    {"job-runners", sample.jobs.runners},
                    {"job-cells", sample.jobs.cells}};
    sample.db = store_.db->stats();
    sample.caches = {{"dem-blocks", terrain_.blocks->stats()},
                     {"results-memory", resultStore_.cache->memoryStats()},
                     {"results-table", resultStore_.cache->tableCounts(), false}};
    PrometheusText out;
    httpMetrics_.write(out);
    writeServerMetrics(out, sample);
    return out.text();
}

json Application::moduleList(bool admin) {
    json out = store_.builtins->list();
    json installed;
    {
        std::shared_lock gate(modules_.runner->gate());
        installed = modules_.registry->list();
    }
    for (json& m : installed) {
        m["builtIn"] = false;
        // Alpha bundles are for the admin only. A browser imports bundles without headers, so
        // the admin's listing carries a short-lived signed grant in the URL instead.
        if (m.value("lifecycle", "") == "alpha" && m.contains("ui")) {
            if (!admin) {
                m.erase("ui");
            } else {
                const std::string grant =
                    grants_.issue(m.at("id").get<std::string>(), Clock::now() + std::chrono::hours(12));
                m["ui"]["bundle"] = m["ui"]["bundle"].get<std::string>() + "?grant=" + grant;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

json Application::mapConfig() const {
    // tools/tiles-builder output names, served under /tiles by the web container. A `map` object in
    // the config is merged over these (RFC 7386), so an operator changes only what differs.
    json out = {{"basemap", {{"pmtiles", "/tiles/basemap.pmtiles"}}},
                {"satellite", {{"pmtiles", "/tiles/satellite.pmtiles"}}},
                {"hillshade", {{"pmtiles", "/tiles/hillshade.pmtiles"}}},
                {"contours", {{"pmtiles", "/tiles/contours.pmtiles"}, {"sourceLayer", "contours"}}},
                {"glyphs", "/tiles/fonts/{fontstack}/{range}.pbf"},
                {"zoom", 11}};
    // The satellite credit is the operator's: it comes from the manifest the tiles-builder wrote.
    if (!config_.data.dem.empty()) {
        if (const auto attributions = manifestAttributions(config_.data.dem); !attributions.empty()) {
            out["attribution"] = attributions;
        }
        dropUnbuiltLayers(out, config_.data.dem);
    }
    if (terrain_.dem) {
        const rfcore::GeoPoint c = terrain_.dem->raster().centre();
        out["center"] = {c.longitudeDeg, c.latitudeDeg};
    }
    if (config_.raw.contains("map")) out.merge_patch(config_.raw.at("map"));
    return out;
}

std::string Application::client(const Req& req) const {
    if (config_.server.trustProxy && req.has_header("X-Forwarded-For")) {
        // The proxy appends the address it saw; earlier entries are the client's own claims.
        const std::string xff = req.get_header_value("X-Forwarded-For");
        const auto comma = xff.rfind(',');
        std::string last = comma == std::string::npos ? xff : xff.substr(comma + 1);
        last.erase(0, last.find_first_not_of(' '));
        if (!last.empty()) return last;
    }
    return req.remote_addr;
}

json Application::body(const Req& req) const {
    if (req.body.size() > config_.server.maxBodyBytes) throw HttpError(413, "request body too large");
    try {
        return json::parse(req.body);
    } catch (const json::exception& e) {
        throw HttpError(400, std::string("body is not valid JSON: ") + e.what());
    }
}

Principal Application::session(const Req& req) const { return store_.auth->authenticate(bearer(req)); }

bool Application::isAdmin(const Req& req) const {
    const std::string t = bearer(req);
    return !config_.adminToken.empty() && !t.empty() && Secrets::constantTimeEqual(t, config_.adminToken);
}

void Application::requireAdmin(const Req& req) {
    limiter_->take("admin", client(req));
    if (config_.adminToken.empty()) throw HttpError(403, "the admin API is disabled on this server");
    if (!isAdmin(req)) throw HttpError(401, "admin token required");
}

/// Health, missions, links, calculations, jobs and the elevation profile.
struct Application::MissionApi {
    Application& app;
    void getHealth(const Req& req, Res& res) const;
    void postMission(const Req& req, Res& res) const;
    void getMission(const Req& req, Res& res) const;
    void putMission(const Req& req, Res& res) const;
    void getLinks(const Req& req, Res& res) const;
    void postLink(const Req& req, Res& res) const;
    void deleteLink(const Req& req, Res& res) const;
    void getLink(const Req& req, Res& res) const;
    void postLinkOpen(const Req& req, Res& res) const;
    void postCalcPath(const Req& req, Res& res) const;
    void postCalcRoute(const Req& req, Res& res) const;
    void postCalcCoverage(const Req& req, Res& res) const;
    void getJob(const Req& req, Res& res) const;
    void deleteJob(const Req& req, Res& res) const;
    void getJobGrid(const Req& req, Res& res) const;
    void getElevationProfile(const Req& req, Res& res) const;
};

/// The library, weather, map configuration and the public module list and files.
struct Application::DataApi {
    Application& app;
    void getLibraryList(const Req& req, Res& res) const;
    void getLibraryItem(const Req& req, Res& res) const;
    void getLibraryHistory(const Req& req, Res& res) const;
    void postLibraryItem(const Req& req, Res& res) const;
    void putLibraryItem(const Req& req, Res& res) const;
    void getWeatherPoint(const Req& req, Res& res) const;
    void getWeatherHorizon(const Req& req, Res& res) const;
    void getMapConfig(const Req& req, Res& res) const;
    void getModules(const Req& req, Res& res) const;
    void getModuleFile(const Req& req, Res& res) const;
};

/// The admin API: modules, trusted keys and caches.
struct Application::AdminApi {
    Application& app;
    void postAdminModule(const Req& req, Res& res) const;
    void putAdminModule(const Req& req, Res& res) const;
    void deleteAdminModule(const Req& req, Res& res) const;
    void getAdminNotices(const Req& req, Res& res) const;
    void getAdminKeys(const Req& req, Res& res) const;
    void postAdminKey(const Req& req, Res& res) const;
    void deleteAdminKey(const Req& req, Res& res) const;
    void getAdminCache(const Req& req, Res& res) const;
    void putAdminCache(const Req& req, Res& res) const;
    void deleteAdminCache(const Req& req, Res& res) const;
    void getAdminMetrics(const Req& req, Res& res) const;
};

/// Flight logs: decode a file to preview it, and the flights kept in a mission.
struct Application::FlightLogApi {
    Application& app;
    void postDecode(const Req& req, Res& res) const;
    void getList(const Req& req, Res& res) const;
    void postImport(const Req& req, Res& res) const;
    void getFlight(const Req& req, Res& res) const;
    void deleteFlight(const Req& req, Res& res) const;

    /// The session, checked against the mission in the path (and for editing when `edit`).
    [[nodiscard]] Principal principal(const Req& req, bool edit) const {
        const Principal p = app.session(req);
        if (p.missionId != req.matches[1].str()) throw HttpError(403, "this session belongs to another mission");
        if (edit && p.role != Role::Edit) throw HttpError(403, "this is a read-only link");
        return p;
    }
    [[nodiscard]] static std::string fileName(const Req& req) {
        return req.has_param("fileName") ? req.get_param_value("fileName") : "flight.log";
    }
};

template <typename Run>
void Application::dispatch(const std::string& rateClass, const Run& run, const Req& req, Res& res) {
    try {
        limiter_->take(rateClass, client(req));
        run();
    } catch (const HttpError& e) {
        sendHttpError(res, e);
    } catch (const json::exception& e) {
        sendJson(res, 400, {{"message", std::string("malformed request: ") + e.what()}});
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): HTTP boundary, any other failure is a logged 500
        internalError(req, res, e.what());
    }
    // A non-std exception reaches the server's exception handler set in routes().
}

std::unique_ptr<PinGate> Application::makePinGate() {
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

void Application::internalError(const Req& req, Res& res, std::string_view what) {
    log_.error("http", std::format("{} {}: {}", req.method, redacted(req.path), what));
    sendJson(res, 500, {{"message", "internal error"}});
}

template <typename Api, typename Method>
auto Application::route(std::string rateClass, Method method) {
    return [this, rateClass = std::move(rateClass), method](const Req& req, Res& res) {
        const auto started = std::chrono::steady_clock::now();
        dispatch(rateClass, [this, method, &req, &res] { (Api{*this}.*method)(req, res); }, req, res);
        httpMetrics_.record(rateClass, req.method, res.status,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    };
}

void Application::routes() {
    httplib::Server& s = *http_;
    const int threads = config_.server.threads;
    // httplib takes ownership of the queue it gets and deletes it when the server stops.
    s.new_task_queue = [this, threads] {
        return std::make_unique<CountingTaskQueue>(static_cast<std::size_t>(threads), httpMetrics_).release();
    };
    s.set_payload_max_length(config_.server.maxUploadBytes);
    s.set_read_timeout(30, 0);
    s.set_write_timeout(120, 0);
    s.set_logger([this](const Req& req, const Res& res) {
        log_.info("http", std::format("{} {} {}", req.method, redacted(req.path), res.status));
    });
    // Errors dispatch does not answer itself; without a handler httplib would put what() in a header.
    s.set_exception_handler([this](const Req& req, Res& res, const std::exception_ptr&) {
        internalError(req, res, "unexpected exception");
    });
    s.set_error_handler([](const Req&, Res& res) {
        if (res.body.empty()) sendJson(res, res.status, {{"message", httplib::status_message(res.status)}});
    });

    // Every handler runs inside dispatch: rate limit, uniform JSON errors, Retry-After on 429 and 503.
    const std::string api = "/api/v1";
    const std::string mission = api + R"(/missions/([A-Za-z0-9_-]+))";
    const std::string library = api + R"(/library/([a-z-]+))";
    const std::string libraryItem = library + R"(/([a-z0-9._-]+))";

    s.Get(api + "/health", route<MissionApi>("default", &MissionApi::getHealth));

    // ---- missions ----
    s.Post(api + "/missions", route<MissionApi>("create", &MissionApi::postMission));
    s.Get(mission, route<MissionApi>("default", &MissionApi::getMission));
    s.Put(mission, route<MissionApi>("default", &MissionApi::putMission));

    // ---- links (owner key) ----
    s.Get(mission + "/links", route<MissionApi>("default", &MissionApi::getLinks));
    s.Post(mission + "/links", route<MissionApi>("default", &MissionApi::postLink));
    s.Delete(mission + R"(/links/([A-Za-z0-9_-]+))", route<MissionApi>("default", &MissionApi::deleteLink));
    // Public, before the PIN: what the PIN screen shows. Same rate class as opening.
    s.Get(api + R"(/links/([A-Za-z0-9_-]+))", route<MissionApi>("open", &MissionApi::getLink));
    s.Post(api + R"(/links/([A-Za-z0-9_-]+)/open)", route<MissionApi>("open", &MissionApi::postLinkOpen));

    // ---- calc ----
    s.Post(api + "/calc/path", route<MissionApi>("calc", &MissionApi::postCalcPath));
    s.Post(api + "/calc/route", route<MissionApi>("calc", &MissionApi::postCalcRoute));
    s.Post(api + "/calc/coverage", route<MissionApi>("coverage", &MissionApi::postCalcCoverage));
    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+))", route<MissionApi>("default", &MissionApi::getJob));
    s.Delete(api + R"(/jobs/([A-Za-z0-9_-]+))", route<MissionApi>("default", &MissionApi::deleteJob));
    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+)/coverage/(control|video)\.json)", route<MissionApi>("default", &MissionApi::getJobGrid));
    s.Get(api + "/elevation/profile", route<MissionApi>("calc", &MissionApi::getElevationProfile));

    // ---- flight logs ----
    const std::string flightLogs = mission + "/flightlogs";
    s.Post(api + "/flightlogs/decode", route<FlightLogApi>("calc", &FlightLogApi::postDecode));
    s.Get(flightLogs, route<FlightLogApi>("default", &FlightLogApi::getList));
    s.Post(flightLogs, route<FlightLogApi>("calc", &FlightLogApi::postImport));
    s.Get(flightLogs + R"(/([A-Za-z0-9_-]+))", route<FlightLogApi>("default", &FlightLogApi::getFlight));
    s.Delete(flightLogs + R"(/([A-Za-z0-9_-]+))", route<FlightLogApi>("default", &FlightLogApi::deleteFlight));

    // ---- library ----
    s.Get(library, route<DataApi>("default", &DataApi::getLibraryList));
    s.Get(libraryItem, route<DataApi>("default", &DataApi::getLibraryItem));
    s.Get(libraryItem + "/history", route<DataApi>("default", &DataApi::getLibraryHistory));
    s.Post(library, route<DataApi>("admin", &DataApi::postLibraryItem));
    s.Put(libraryItem, route<DataApi>("admin", &DataApi::putLibraryItem));

    // ---- weather ----
    s.Get(api + "/weather/point", route<DataApi>("default", &DataApi::getWeatherPoint));
    s.Get(api + "/weather/horizon", route<DataApi>("default", &DataApi::getWeatherHorizon));

    s.Get(api + "/config/map", route<DataApi>("default", &DataApi::getMapConfig));

    // ---- modules ----
    s.Get(api + "/modules", route<DataApi>("default", &DataApi::getModules));
    s.Get(api + R"(/modules/([a-z0-9._-]+)/(.+))", route<DataApi>("default", &DataApi::getModuleFile));

    // ---- admin ----
    s.Post(api + "/admin/modules", route<AdminApi>("admin", &AdminApi::postAdminModule));
    s.Put(api + R"(/admin/modules/([a-z0-9._-]+))", route<AdminApi>("admin", &AdminApi::putAdminModule));
    s.Delete(api + R"(/admin/modules/([a-z0-9._-]+))", route<AdminApi>("admin", &AdminApi::deleteAdminModule));
    s.Get(api + "/admin/modules/notices", route<AdminApi>("admin", &AdminApi::getAdminNotices));
    s.Get(api + "/admin/keys", route<AdminApi>("admin", &AdminApi::getAdminKeys));
    s.Post(api + "/admin/keys", route<AdminApi>("admin", &AdminApi::postAdminKey));
    s.Delete(api + R"(/admin/keys/([0-9a-f]+))", route<AdminApi>("admin", &AdminApi::deleteAdminKey));
    s.Get(api + "/admin/cache", route<AdminApi>("admin", &AdminApi::getAdminCache));
    s.Put(api + "/admin/cache", route<AdminApi>("admin", &AdminApi::putAdminCache));
    s.Delete(api + "/admin/cache", route<AdminApi>("admin", &AdminApi::deleteAdminCache));
    s.Get(api + "/admin/metrics", route<AdminApi>("admin", &AdminApi::getAdminMetrics));
}

// ---- flight logs ----

void Application::FlightLogApi::postDecode(const Req& req, Res& res) const {
    (void)app.session(req);
    sendJson(res, 200, app.flightLogs_->preview(req.body, fileName(req)));
}

void Application::FlightLogApi::getList(const Req& req, Res& res) const {
    const Principal p = principal(req, false);
    sendJson(res, 200, app.flightLogs_->list(p.missionId));
}

void Application::FlightLogApi::postImport(const Req& req, Res& res) const {
    const Principal p = principal(req, true);
    sendJson(res, 201, app.flightLogs_->importFile(p.missionId, req.body, fileName(req)));
}

void Application::FlightLogApi::getFlight(const Req& req, Res& res) const {
    const Principal p = principal(req, false);
    const auto m = app.store_.missions->get(p.missionId);
    if (!m) throw HttpError(404, "no such mission");
    sendJson(res, 200, app.flightLogs_->view(*m, req.matches[2]));
}

void Application::FlightLogApi::deleteFlight(const Req& req, Res& res) const {
    const Principal p = principal(req, true);
    app.flightLogs_->remove(p.missionId, req.matches[2]);
    res.status = 204;
}

void Application::MissionApi::getHealth(const Req&, Res& res) const {
    sendJson(res, 200,
             {{"ok", true},
              {"version", RFMAP_HOST_VERSION},
              {"dem", app.terrain_.dem != nullptr},
              {"landcover", app.terrain_.landcover != nullptr},
              {"weather", app.forecast_.service->configured()}});
}

// ---- missions ----

void Application::MissionApi::postMission(const Req& req, Res& res) const {
    const json doc = app.body(req);
    app.store_.schemas->require("mission", doc);
    const AuthService::OwnerKey key = AuthService::newOwnerKey();
    const std::string id = app.store_.missions->create(doc, key.hash);
    const std::string sess = app.store_.auth->createSession(id, Role::Edit, std::nullopt);
    res.set_header("ETag", "\"1\"");
    sendJson(res, 201, {{"missionId", id}, {"ownerKey", key.plain}, {"session", sess}, {"revision", 1}});
}

void Application::MissionApi::getMission(const Req& req, Res& res) const {
    const Principal p = app.session(req);
    const std::string id = req.matches[1];
    if (p.missionId != id) throw HttpError(403, "this session belongs to another mission");
    const auto m = app.store_.missions->get(id);
    if (!m) throw HttpError(404, "no such mission");
    json out = {{"mission", m->doc}, {"role", toString(p.role)}, {"revision", m->revision}};
    if (m->lastResult.has_value()) out["lastResult"] = *m->lastResult;
    res.set_header("ETag", std::format("\"{}\"", m->revision));
    sendJson(res, 200, out);
}

void Application::MissionApi::putMission(const Req& req, Res& res) const {
    const Principal p = app.session(req);
    const std::string id = req.matches[1];
    if (p.missionId != id) throw HttpError(403, "this session belongs to another mission");
    if (p.role != Role::Edit) throw HttpError(403, "this is a read-only link");
    const int expected = ifMatchRevision(req);
    const json doc = app.body(req);
    app.store_.schemas->require("mission", doc);
    const auto rev = app.store_.missions->save(id, doc, expected);
    if (!rev.has_value()) {
        const auto cur = app.store_.missions->get(id);
        if (!cur) throw HttpError(404, "no such mission");
        throw HttpError(409, "someone saved this mission in between", {{"revision", cur->revision}});
    }
    res.set_header("ETag", std::format("\"{}\"", *rev));
    sendJson(res, 200, {{"revision", *rev}});
}

// ---- links (owner key) ----

void Application::MissionApi::getLinks(const Req& req, Res& res) const {
    json out = json::array();
    for (const auto& l : app.store_.auth->listLinks(req.matches[1], header(req, "X-Owner-Key"))) out.push_back(AuthService::linkJson(l));
    sendJson(res, 200, out);
}

void Application::MissionApi::postLink(const Req& req, Res& res) const {
    const std::string id = req.matches[1];
    const std::string key = header(req, "X-Owner-Key");
    app.store_.auth->requireOwner(id, key);  // before looking at the body, so strangers learn nothing
    const json b = app.body(req);
    app.store_.schemas->require("link-create", b);
    std::optional<TimePoint> expires;
    if (b.contains("expiresAt")) {
        expires = parseIsoUtc(b.at("expiresAt").get<std::string>());
        if (!expires.has_value()) throw HttpError(422, "expiresAt is not ISO 8601 UTC");
    }
    const auto created =
        app.store_.auth->createLink(id, key, roleFromString(b.at("role").get<std::string>()),
                                 b.at("pin").get<std::string>(), b.value("label", ""), expires);
    json out = AuthService::linkJson(created.record);
    out["token"] = created.token;
    sendJson(res, 201, out);
}

void Application::MissionApi::deleteLink(const Req& req, Res& res) const {
    app.store_.auth->revokeLink(req.matches[1], header(req, "X-Owner-Key"), req.matches[2]);
    res.status = 204;
}

void Application::MissionApi::getLink(const Req& req, Res& res) const {
    const LinkRecord l = app.store_.auth->peekLink(req.matches[1]);
    const auto m = app.store_.missions->get(l.missionId);
    json out = {{"role", toString(l.role)}, {"missionName", m ? m->doc.value("name", "") : ""}};
    if (l.expiresAt.has_value()) out["expiresAt"] = isoUtc(*l.expiresAt);
    if (l.lockedUntil.has_value() && *l.lockedUntil > app.clock_.now()) out["pausedUntil"] = isoUtc(*l.lockedUntil);
    sendJson(res, 200, out);
}

void Application::MissionApi::postLinkOpen(const Req& req, Res& res) const {
    const json b = app.body(req);
    if (!b.contains("pin") || !b.at("pin").is_string()) throw HttpError(400, "pin is required");
    const auto opened = app.store_.auth->openLink(req.matches[1], b.at("pin"));
    const auto m = app.store_.missions->get(opened.missionId);
    sendJson(res, 200, {{"session", opened.session}, {"missionId", opened.missionId}, {"role", toString(opened.role)},
                        {"missionName", m ? m->doc.value("name", "") : ""}});
}

// ---- calc ----

void Application::MissionApi::postCalcPath(const Req& req, Res& res) const {
    (void)app.session(req);
    const json b = app.body(req);
    app.store_.schemas->require("calc-path", b);
    const std::shared_lock gate(app.modules_.runner->gate());
    sendJson(res, 200, app.calc_->path(b));
}

void Application::MissionApi::postCalcRoute(const Req& req, Res& res) const {
    const Principal p = app.session(req);
    const json mission = app.body(req);
    app.store_.schemas->require("mission", mission);
    std::optional<int> revision;
    if (const auto stored = app.store_.missions->get(p.missionId); stored.has_value() && stored->doc == mission) {
        revision = stored->revision;
    }
    json result;
    {
        const std::shared_lock gate(app.modules_.runner->gate());
        result = app.calc_->route(mission, revision);
    }
    if (revision.has_value() && p.role == Role::Edit) app.store_.missions->storeResultIfCurrent(p.missionId, mission, result);
    sendJson(res, 200, result);
}

void Application::MissionApi::postCalcCoverage(const Req& req, Res& res) const {
    const Principal p = app.session(req);
    const json mission = app.body(req);
    app.store_.schemas->require("mission", mission);
    const std::size_t cells = Calculator::coverageCells(mission);
    if (cells == 0) throw HttpError(422, "coverage needs coverage.radiusM, coverage.cellM and coverage.droneAglM");
    if (cells > app.config_.coverage.maxCells) {
        throw HttpError(422, std::format("the grid would have {} cells; this server allows {}. Use a larger cellM or a "
                                         "smaller radius.",
                                         cells, app.config_.coverage.maxCells));
    }
    if (app.jobs_->running() >= static_cast<std::size_t>(app.config_.coverage.maxJobs) * 4) {
        throw HttpError(503, "too many coverage jobs queued; try again shortly", {{"retryAfterS", 30}});
    }
    sendJson(res, 202, app.jobs_->start(p.missionId, mission));
}

void Application::MissionApi::getJob(const Req& req, Res& res) const {
    sendJson(res, 200, app.jobs_->status(app.session(req).missionId, req.matches[1]));
}

void Application::MissionApi::deleteJob(const Req& req, Res& res) const {
    app.jobs_->cancel(app.session(req).missionId, req.matches[1]);
    res.status = 204;
}

void Application::MissionApi::getJobGrid(const Req& req, Res& res) const {
    sendJson(res, 200, app.jobs_->grid(app.session(req).missionId, req.matches[1], req.matches[2]));
}

void Application::MissionApi::getElevationProfile(const Req& req, Res& res) const {
    if (!req.has_param("from") || !req.has_param("to")) throw HttpError(400, "from and to are required (lat,lon)");
    const auto from = parseLatLon(req.get_param_value("from"));
    const auto to = parseLatLon(req.get_param_value("to"));
    const DemSource& dem = app.links_->requireDem();
    const auto built = rfcore::ProfileBuilder(dem).build(from, to);
    if (!built.profile.has_value()) {
        throw HttpError(422, "missing elevation data on part of the path; nothing is guessed",
                        app.links_->missingData(built.missing, from, to));
    }
    const auto& smp = built.profile->samples();
    sendJson(res, 200, LinkService::profileJson(*built.profile, (smp[1].distance - smp[0].distance).value()));
}

// ---- library ----

std::string Application::libraryKind(const std::string& kind) const {
    (void)SchemaRegistry::libraryFile(kind);
    if (kind == "radio-profiles" && !store_.builtins->enabled(BuiltinModules::kRadioProfiles)) {
        throw HttpError(404, "radio profiles (rf.radio-profiles) are disabled on this server");
    }
    return kind;
}

void Application::DataApi::getLibraryList(const Req& req, Res& res) const {
    sendJson(res, 200, app.store_.library->list(app.libraryKind(req.matches[1])));
}

void Application::DataApi::getLibraryItem(const Req& req, Res& res) const {
    const auto item = app.store_.library->get(app.libraryKind(req.matches[1]), req.matches[2]);
    if (!item) throw HttpError(404, "no such library item");
    sendJson(res, 200, *item);
}

void Application::DataApi::getLibraryHistory(const Req& req, Res& res) const {
    sendJson(res, 200, app.store_.library->history(app.libraryKind(req.matches[1]), req.matches[2]));
}

void Application::DataApi::postLibraryItem(const Req& req, Res& res) const {
    app.requireAdmin(req);
    sendJson(res, 201, app.store_.library->add(app.libraryKind(req.matches[1]), app.body(req), "admin@" + app.client(req)));
}

void Application::DataApi::putLibraryItem(const Req& req, Res& res) const {
    app.requireAdmin(req);
    sendJson(res, 200, app.store_.library->update(app.libraryKind(req.matches[1]), req.matches[2], app.body(req), "admin@" + app.client(req)));
}

// ---- weather ----

void Application::DataApi::getWeatherPoint(const Req& req, Res& res) const {
    const double lat = queryNumber(req, "lat");
    const double lon = queryNumber(req, "lon");
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) throw HttpError(400, "lat/lon out of range");
    TimePoint t = Clock::now();
    if (req.has_param("timeUtc")) {
        const auto parsed = parseIsoUtc(req.get_param_value("timeUtc"));
        if (!parsed) throw HttpError(400, "timeUtc is not ISO 8601");
        t = *parsed;
    }
    const ForecastAt f = app.forecast_.service->at(lat, lon, t);
    json out = f.point;
    out["notes"] = json::array();
    for (const auto& n : f.notes) out["notes"].push_back({{"what", n.what}, {"detail", n.detail}});
    sendJson(res, 200, out);
}

void Application::DataApi::getWeatherHorizon(const Req&, Res& res) const {
    const rfcore::GeoPoint probe = app.terrain_.dem ? app.terrain_.dem->raster().centre() : rfcore::GeoPoint{0.0, 0.0};
    sendJson(res, 200, app.forecast_.service->horizon(probe.latitudeDeg, probe.longitudeDeg));
}

void Application::DataApi::getMapConfig(const Req&, Res& res) const { sendJson(res, 200, app.mapConfig()); }

// ---- modules ----

void Application::DataApi::getModules(const Req& req, Res& res) const { sendJson(res, 200, app.moduleList(app.isAdmin(req))); }

void Application::DataApi::getModuleFile(const Req& req, Res& res) const {
    const std::string id = req.matches[1];
    const std::string path = req.matches[2];
    std::optional<std::filesystem::path> file;
    bool alpha = false;
    {
        const std::shared_lock gate(app.modules_.runner->gate());
        // publicFile serves ui/ and the options schema only, never lib/.
        file = app.modules_.registry->publicFile(id, path);
        if (const auto* m = app.modules_.registry->instance(id)) alpha = m->manifest().lifecycle == rfmod::Lifecycle::Alpha;
    }
    if (!file.has_value()) throw HttpError(404, "no such module file");
    // An absent grant reads as empty, which is never valid.
    if (alpha && !app.isAdmin(req) && !app.grants_.valid(id, req.get_param_value("grant"), Clock::now())) {
        throw HttpError(404, "no such module file");
    }
    const std::ifstream in(*file, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Cache-Control", alpha ? "private, no-store" : "public, max-age=300");
    res.status = 200;
    res.set_content(content.str(), moduleFileType(file->extension().string()));
}

// ---- admin ----

void Application::AdminApi::postAdminModule(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const bool install = !req.has_param("install") || req.get_param_value("install") != "false";
    rfmod::ValidationReport report;
    {
        const std::unique_lock gate(app.modules_.runner->gate());
        report = app.modules_.registry->upload(req.body, install);
    }
    app.log_.info("modules", std::string("upload ") + (report.ok() ? "passed" : "refused") + (install ? ", install" : ", check only"));
    sendJson(res, 200, report.toJson());
}

void Application::AdminApi::putAdminModule(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const std::string id = req.matches[1];
    const json b = app.body(req);
    std::optional<bool> enabled;
    if (b.contains("enabled")) enabled = b.at("enabled").get<bool>();
    std::optional<json> options;
    if (b.contains("options")) options = b.at("options");
    if (app.store_.builtins->isBuiltin(id)) {
        app.store_.builtins->set(id, enabled, options);
    } else {
        const std::unique_lock gate(app.modules_.runner->gate());
        try {
            if (options.has_value()) app.modules_.registry->setOptions(id, *options);
            if (enabled.has_value()) app.modules_.registry->setEnabled(id, *enabled);
        } catch (const std::invalid_argument& e) {
            throw HttpError(422, e.what());
        }
    }
    sendJson(res, 200, app.moduleList(true));
}

void Application::AdminApi::deleteAdminModule(const Req& req, Res& res) const {
    app.requireAdmin(req);
    if (app.store_.builtins->isBuiltin(req.matches[1])) throw HttpError(422, "built-in modules can be disabled, not removed");
    const std::unique_lock gate(app.modules_.runner->gate());
    try {
        app.modules_.registry->remove(req.matches[1]);
    } catch (const std::invalid_argument& e) {
        throw HttpError(422, e.what());
    }
    res.status = 204;
}

void Application::AdminApi::getAdminNotices(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const std::shared_lock gate(app.modules_.runner->gate());
    sendJson(res, 200, app.modules_.registry->notices());
}

json Application::trustedKeys() const {
    json out = json::array();
    for (const auto& k : modules_.trust->keys()) {
        out.push_back({{"id", k.key.id()}, {"label", k.label}, {"key", k.key.toText()}, {"builtIn", k.builtIn},
                       {"firstParty", k.firstParty()}});
    }
    return out;
}

void Application::AdminApi::getAdminKeys(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const std::shared_lock gate(app.modules_.runner->gate());
    sendJson(res, 200, app.trustedKeys());
}

void Application::AdminApi::postAdminKey(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const json b = app.body(req);
    const std::unique_lock gate(app.modules_.runner->gate());
    try {
        app.modules_.trust->add(rfmod::PublicKey::fromText(b.at("key").get<std::string>()), b.value("label", ""));
    } catch (const std::invalid_argument& e) {
        throw HttpError(422, e.what());
    }
    app.modules_.registry->reload();
    sendJson(res, 201, app.trustedKeys());
}

void Application::AdminApi::deleteAdminKey(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const std::unique_lock gate(app.modules_.runner->gate());
    try {
        app.modules_.trust->remove(req.matches[1]);
    } catch (const std::invalid_argument& e) {
        throw HttpError(409, e.what());
    }
    // Modules signed with the removed key stop loading now, not only at the next start.
    app.modules_.registry->reload();
    res.status = 204;
}

void Application::AdminApi::getAdminCache(const Req& req, Res& res) const {
    app.requireAdmin(req);
    sendJson(res, 200, app.cacheTiers());
}

void Application::AdminApi::putAdminCache(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const json b = app.body(req);
    if (!b.is_array()) throw HttpError(422, "send [{id, limitBytes?, limitRows?}]");
    ResultCache::Limits l = app.resultStore_.cache->limits();
    std::size_t demLimit = app.terrain_.blocks->stats().limitBytes;
    for (const json& t : b) {
        if (const std::string id = t.at("id"); id == "dem-blocks") demLimit = t.value("limitBytes", demLimit);
        else if (id == "results-memory") l.memoryBytes = t.value("limitBytes", l.memoryBytes);
        else if (id == "results-table") {
            l.tableBytes = t.value("limitBytes", l.tableBytes);
            l.tableRows = t.value("limitRows", l.tableRows);
        } else throw HttpError(422, "unknown cache tier " + id);
    }
    app.terrain_.blocks->setLimit(demLimit);
    app.resultStore_.cache->setLimits(l);
    const json stored = {{"demBlocksBytes", demLimit}, {"resultMemoryBytes", l.memoryBytes},
                         {"resultTableRows", l.tableRows}, {"resultTableBytes", l.tableBytes}};
    app.store_.db->tx([&stored](pqxx::work& w) {
        w.exec("insert into settings(key, value) values ('cache.limits', $1::jsonb) "
               "on conflict (key) do update set value = excluded.value",
               pqxx::params{stored.dump()});
    });
    sendJson(res, 200, app.cacheTiers());
}

void Application::AdminApi::deleteAdminCache(const Req& req, Res& res) const {
    app.requireAdmin(req);
    const std::string tier = req.has_param("tier") ? req.get_param_value("tier") : "all";
    if (tier == "dem-blocks" || tier == "all") app.terrain_.blocks->clear();
    if (tier == "results-memory" || tier == "all") app.resultStore_.cache->clearMemory();
    if (tier == "results-table" || tier == "all") app.resultStore_.cache->clearTable();
    if (tier != "all" && tier != "dem-blocks" && tier != "results-memory" && tier != "results-table") {
        throw HttpError(422, "unknown cache tier " + tier);
    }
    res.status = 204;
}

void Application::AdminApi::getAdminMetrics(const Req& req, Res& res) const {
    app.requireAdmin(req);
    if (!app.config_.metrics.enabled) throw HttpError(404, "metrics are off; set metrics.enabled in the server config");
    res.status = 200;
    res.set_content(app.metricsText(), "text/plain; version=0.0.4; charset=utf-8");
}

}  // namespace rfmap
