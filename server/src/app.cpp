#include "app.hpp"

#include <httplib.h>

#include <fstream>
#include <regex>
#include <shared_mutex>
#include <sstream>

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
    const std::string h = header(req, "Authorization");
    if (h.starts_with("Bearer ")) return h.substr(7);
    return {};
}

/// Paths carry link tokens and module ids; the token must never reach the log.
std::string redacted(const std::string& path) {
    static const std::regex link("/links/[^/]+(/open)?$");
    return std::regex_replace(path, link, "/links/***$1");
}

rfcore::GeoPoint parseLatLon(const std::string& text) {
    static const std::regex re(R"(^\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*,\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)\s*$)");
    std::smatch m;
    if (!std::regex_match(text, m, re)) throw HttpError(400, "expected lat,lon, got '" + text + "'");
    const double lat = std::stod(m[1]), lon = std::stod(m[2]);
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

    db_ = std::make_unique<Database>(config_.database.url, config_.database.poolSize);
    for (const auto& m : db_->migrate(sqlDir)) log_.info("startup", "applied migration " + m);

    schemas_ = std::make_unique<SchemaRegistry>(config_.schemasDir);
    schemas_->addInline("calc-path", calcPathSchema());
    schemas_->addInline("link-create", {{"type", "object"},
                                        {"required", {"role", "pin"}},
                                        {"properties",
                                         {{"role", {{"enum", {"edit", "view"}}}},
                                          {"pin", {{"type", "string"}, {"pattern", "^[0-9]{6}$"}}},
                                          {"label", {{"type", "string"}, {"maxLength", 120}}},
                                          {"expiresAt", {{"type", "string"}, {"format", "date-time"}}}}}});

    authStore_ = std::make_unique<PgAuthStore>(*db_);
    pins_ = std::make_unique<PinHasher>(PinHasher::costFromName(config_.auth.argon2Cost));
    auth_ = std::make_unique<AuthService>(
        *authStore_, *pins_, clock_,
        AuthService::Settings{config_.auth.pinMaxAttempts, std::chrono::minutes(config_.auth.pinLockoutMinutes),
                              std::chrono::hours(config_.auth.sessionTtlHours)});
    missions_ = std::make_unique<MissionStore>(*db_);
    library_ = std::make_unique<Library>(*db_, *schemas_);
    builtins_ = std::make_unique<BuiltinModules>(db_.get(), *schemas_, RFMAP_HOST_VERSION);
    if (builtins_->enabled(BuiltinModules::kRadioProfiles)) {
        for (const auto& line : library_->seed(config_.seedDir)) log_.info("library", line);
    }

    blocks_ = std::make_unique<BlockCache>(config_.cache.demBlocksMb << 20, [](const RasterBlock& b) {
        return b.values.size() * sizeof(float) + sizeof(RasterBlock);
    });
    if (!config_.data.dem.empty()) {
        auto raster = std::make_shared<GdalRaster>(config_.data.dem, *blocks_);
        std::string name = config_.data.demName;
        if (name.empty()) name = manifestSourceName(config_.data.dem);
        if (name.empty()) name = config_.data.dem.filename().string();
        dem_ = std::make_shared<DemSource>(raster, name, config_.data.demStatedErrorM, config_.data.demStatedErrorSource);
        log_.info("terrain", "DEM " + config_.data.dem.string() + " (" + raster->dataType() + ", step " +
                                 std::to_string(raster->nativeResolution().value()) + " m): " + name);
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
        auto raster = std::make_shared<GdalRaster>(config_.data.landcover, *blocks_);
        std::string name = config_.data.landcoverName;
        if (name.empty()) name = manifestSourceName(config_.data.landcover);
        if (name.empty()) name = config_.data.landcover.filename().string();
        landcover_ = std::make_shared<LandCoverSource>(raster, name);
    }

    resultTable_ = std::make_unique<PgResultTable>(*db_);
    results_ = std::make_unique<ResultCache>(
        ResultCache::Limits{config_.cache.resultMemoryMb << 20, config_.cache.resultTableRows,
                            config_.cache.resultTableMb << 20},
        resultTable_.get());
    loadCacheSettings();

    if (!config_.weather.baseUrl.empty()) forecastSource_ = std::make_unique<OpenMeteoSource>(config_.weather);
    else log_.warn("weather", "weather.baseUrl is not set: no forecasts, k = 4/3 everywhere");
    weather_ = std::make_unique<WeatherService>(config_.weather, forecastSource_.get(), clock_);

    // Modules can call back into the server from init, so the host must be complete before
    // the registry loads anything.
    host_ = std::make_unique<ServerHostServices>(nullptr, nullptr, nullptr, nullptr, log_);
    trust_ = std::make_unique<rfmod::TrustStore>(ownerKeyText, config_.modules.trustStore);
    runner_ = std::make_unique<ModuleRunner>(nullptr);
    links_ = std::make_unique<LinkService>(dem_, landcover_, *library_, *builtins_, runner_.get(), RFMAP_HOST_VERSION);
    calc_ = std::make_unique<Calculator>(*links_, *weather_, *builtins_, *runner_, *results_, log_);
    host_->bind(links_.get(), library_.get(), weather_.get(), [this](const json& m) { return calc_->contextFor(m); },
                schemas_.get());

    rfmod::RegistryConfig rc;
    rc.root = config_.modules.root;
    rc.workerExecutable = config_.modules.worker;
    rc.hostVersion = rfmod::Version::parse(RFMAP_HOST_VERSION);
    rc.isolateAll = config_.modules.isolateAll;
    registry_ = std::make_unique<rfmod::ModuleRegistry>(rc, *trust_, *host_);
    registry_->reload();
    runner_->setRegistry(registry_.get());
    for (const auto& n : registry_->notices()) log_.warn("modules", n);

    jobs_ = std::make_unique<JobManager>(*calc_, config_.coverage.maxJobs, config_.coverage.threads,
                                         config_.coverage.keepMinutes, log_);
    limiter_ = std::make_unique<RateLimiter>(config_.rateLimits, clock_);
    grantSecret_ = Secrets::randomToken(32);
    http_ = std::make_unique<httplib::Server>();
    routes();
}

Application::~Application() {
    if (http_) http_->stop();
    jobs_.reset();
    registry_.reset();
}

bool Application::listen() {
    log_.info("http", "listening on " + config_.server.listen + ":" + std::to_string(config_.server.port));
    return http_->listen(config_.server.listen, config_.server.port);
}

int Application::bindAny() { return http_->bind_to_any_port(config_.server.listen); }
void Application::serveBound() { http_->listen_after_bind(); }
void Application::stop() { http_->stop(); }

void Application::loadCacheSettings() {
    const auto stored = db_->tx([](pqxx::work& w) -> std::optional<json> {
        const auto r = w.exec("select value::text from settings where key = 'cache.limits'");
        if (r.empty()) return std::nullopt;
        return json::parse(r[0][0].as<std::string>());
    });
    if (!stored) return;
    if (stored->contains("demBlocksBytes")) blocks_->setLimit(stored->at("demBlocksBytes").get<std::size_t>());
    ResultCache::Limits l = results_->limits();
    l.memoryBytes = stored->value("resultMemoryBytes", l.memoryBytes);
    l.tableRows = stored->value("resultTableRows", l.tableRows);
    l.tableBytes = stored->value("resultTableBytes", l.tableBytes);
    results_->setLimits(l);
}

json Application::cacheTiers() {
    const CacheStats dem = blocks_->stats();
    const CacheStats mem = results_->memoryStats();
    const auto table = results_->tableStats();
    const auto limits = results_->limits();
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

std::string Application::bundleGrant(const std::string& moduleId, TimePoint until) const {
    const std::string exp = std::to_string(Clock::to_time_t(until));
    return exp + "." + Secrets::mac(grantSecret_, moduleId + "|" + exp);
}

bool Application::grantValid(const std::string& moduleId, const std::string& grant) const {
    const auto dot = grant.find('.');
    if (dot == std::string::npos) return false;
    const std::string exp = grant.substr(0, dot);
    try {
        if (Clock::from_time_t(std::stoll(exp)) < Clock::now()) return false;
    } catch (const std::exception&) {
        return false;
    }
    return Secrets::constantTimeEqual(grant.substr(dot + 1), Secrets::mac(grantSecret_, moduleId + "|" + exp));
}

json Application::moduleList(bool admin) {
    json out = builtins_->list();
    json installed;
    {
        std::shared_lock gate(runner_->gate());
        installed = registry_->list();
    }
    for (json& m : installed) {
        m["builtIn"] = false;
        // Alpha bundles are for the admin only. A browser imports bundles without headers, so
        // the admin's listing carries a short-lived signed grant in the URL instead.
        if (m.value("lifecycle", "") == "alpha" && m.contains("ui")) {
            if (!admin) {
                m.erase("ui");
            } else {
                const std::string grant = bundleGrant(m.at("id"), Clock::now() + std::chrono::hours(12));
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
    if (dem_) {
        const rfcore::GeoPoint c = dem_->raster().centre();
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

Principal Application::session(const Req& req) { return auth_->authenticate(bearer(req)); }

bool Application::isAdmin(const Req& req) const {
    const std::string t = bearer(req);
    return !config_.adminToken.empty() && !t.empty() && Secrets::constantTimeEqual(t, config_.adminToken);
}

void Application::requireAdmin(const Req& req) {
    limiter_->take("admin", client(req));
    if (config_.adminToken.empty()) throw HttpError(403, "the admin API is disabled on this server");
    if (!isAdmin(req)) throw HttpError(401, "admin token required");
}

void Application::routes() {
    httplib::Server& s = *http_;
    const int threads = config_.server.threads;
    s.new_task_queue = [threads] { return new httplib::ThreadPool(static_cast<std::size_t>(threads)); };
    s.set_payload_max_length(config_.server.maxUploadBytes);
    s.set_read_timeout(30, 0);
    s.set_write_timeout(120, 0);
    s.set_logger([this](const Req& req, const Res& res) {
        log_.info("http", req.method + " " + redacted(req.path) + " " + std::to_string(res.status));
    });
    s.set_error_handler([](const Req&, Res& res) {
        if (res.body.empty()) sendJson(res, res.status, {{"message", httplib::status_message(res.status)}});
    });

    // Every handler runs inside this: rate limit, uniform JSON errors, Retry-After on 429.
    using Handler = std::function<void(const Req&, Res&)>;
    auto wrap = [this](const std::string& cls, const Handler& h) {
        return [this, cls, h](const Req& req, Res& res) {
            try {
                limiter_->take(cls, client(req));
                h(req, res);
            } catch (const HttpError& e) {
                json b = e.body();
                b["message"] = e.what();
                if (e.status() == 429 && b.contains("retryAfterS")) {
                    res.set_header("Retry-After", std::to_string(b["retryAfterS"].get<long long>()));
                }
                sendJson(res, e.status(), b);
            } catch (const json::exception& e) {
                sendJson(res, 400, {{"message", std::string("malformed request: ") + e.what()}});
            } catch (const std::exception& e) {
                log_.error("http", req.method + " " + redacted(req.path) + ": " + e.what());
                sendJson(res, 500, {{"message", "internal error"}});
            }
        };
    };
    const std::string api = "/api/v1";

    s.Get(api + "/health", wrap("default", [this](const Req&, Res& res) {
        sendJson(res, 200, {{"ok", true}, {"version", RFMAP_HOST_VERSION}, {"dem", dem_ != nullptr},
                            {"landcover", landcover_ != nullptr}, {"weather", weather_->configured()}});
    }));

    // ---- missions ----
    s.Post(api + "/missions", wrap("create", [this](const Req& req, Res& res) {
        const json doc = body(req);
        schemas_->require("mission", doc);
        const AuthService::OwnerKey key = AuthService::newOwnerKey();
        const std::string id = missions_->create(doc, key.hash);
        const std::string sess = auth_->createSession(id, Role::Edit, std::nullopt);
        res.set_header("ETag", "\"1\"");
        sendJson(res, 201, {{"missionId", id}, {"ownerKey", key.plain}, {"session", sess}, {"revision", 1}});
    }));

    s.Get(api + R"(/missions/([A-Za-z0-9_-]+))", wrap("default", [this](const Req& req, Res& res) {
        const Principal p = session(req);
        const std::string id = req.matches[1];
        if (p.missionId != id) throw HttpError(403, "this session belongs to another mission");
        const auto m = missions_->get(id);
        if (!m) throw HttpError(404, "no such mission");
        json out = {{"mission", m->doc}, {"role", toString(p.role)}, {"revision", m->revision}};
        if (m->lastResult) out["lastResult"] = *m->lastResult;
        res.set_header("ETag", "\"" + std::to_string(m->revision) + "\"");
        sendJson(res, 200, out);
    }));

    s.Put(api + R"(/missions/([A-Za-z0-9_-]+))", wrap("default", [this](const Req& req, Res& res) {
        const Principal p = session(req);
        const std::string id = req.matches[1];
        if (p.missionId != id) throw HttpError(403, "this session belongs to another mission");
        if (p.role != Role::Edit) throw HttpError(403, "this is a read-only link");
        std::string match = header(req, "If-Match");
        match.erase(std::remove_if(match.begin(), match.end(), [](char c) { return c == '"' || c == 'W' || c == '/'; }),
                    match.end());
        int expected = 0;
        try {
            expected = std::stoi(match);
        } catch (const std::exception&) {
            throw HttpError(428, "If-Match with the revision you edited is required");
        }
        const json doc = body(req);
        schemas_->require("mission", doc);
        const auto rev = missions_->save(id, doc, expected);
        if (!rev.has_value()) {
            const auto cur = missions_->get(id);
            if (!cur) throw HttpError(404, "no such mission");
            throw HttpError(409, "someone saved this mission in between", {{"revision", cur->revision}});
        }
        res.set_header("ETag", "\"" + std::to_string(*rev) + "\"");
        sendJson(res, 200, {{"revision", *rev}});
    }));

    // ---- links (owner key) ----
    s.Get(api + R"(/missions/([A-Za-z0-9_-]+)/links)", wrap("default", [this](const Req& req, Res& res) {
        json out = json::array();
        for (const auto& l : auth_->listLinks(req.matches[1], header(req, "X-Owner-Key"))) out.push_back(AuthService::linkJson(l));
        sendJson(res, 200, out);
    }));

    s.Post(api + R"(/missions/([A-Za-z0-9_-]+)/links)", wrap("default", [this](const Req& req, Res& res) {
        const std::string id = req.matches[1];
        const std::string key = header(req, "X-Owner-Key");
        auth_->requireOwner(id, key);  // before looking at the body, so strangers learn nothing
        const json b = body(req);
        schemas_->require("link-create", b);
        std::optional<TimePoint> expires;
        if (b.contains("expiresAt")) {
            expires = parseIsoUtc(b.at("expiresAt").get<std::string>());
            if (!expires) throw HttpError(422, "expiresAt is not ISO 8601 UTC");
        }
        const auto created =
            auth_->createLink(id, key, roleFromString(b.at("role")), b.at("pin"), b.value("label", ""), expires);
        json out = AuthService::linkJson(created.record);
        out["token"] = created.token;
        sendJson(res, 201, out);
    }));

    s.Delete(api + R"(/missions/([A-Za-z0-9_-]+)/links/([A-Za-z0-9_-]+))", wrap("default", [this](const Req& req, Res& res) {
        auth_->revokeLink(req.matches[1], header(req, "X-Owner-Key"), req.matches[2]);
        res.status = 204;
    }));

    // Public, before the PIN: what the PIN screen shows. Same rate class as opening.
    s.Get(api + R"(/links/([A-Za-z0-9_-]+))", wrap("open", [this](const Req& req, Res& res) {
        const LinkRecord l = auth_->peekLink(req.matches[1]);
        const auto m = missions_->get(l.missionId);
        json out = {{"role", toString(l.role)}, {"missionName", m ? m->doc.value("name", "") : ""}};
        if (l.expiresAt) out["expiresAt"] = isoUtc(*l.expiresAt);
        if (l.lockedUntil && *l.lockedUntil > clock_.now()) out["pausedUntil"] = isoUtc(*l.lockedUntil);
        sendJson(res, 200, out);
    }));

    s.Post(api + R"(/links/([A-Za-z0-9_-]+)/open)", wrap("open", [this](const Req& req, Res& res) {
        const json b = body(req);
        if (!b.contains("pin") || !b.at("pin").is_string()) throw HttpError(400, "pin is required");
        const auto opened = auth_->openLink(req.matches[1], b.at("pin"));
        const auto m = missions_->get(opened.missionId);
        sendJson(res, 200, {{"session", opened.session}, {"missionId", opened.missionId}, {"role", toString(opened.role)},
                            {"missionName", m ? m->doc.value("name", "") : ""}});
    }));

    // ---- calc ----
    s.Post(api + "/calc/path", wrap("calc", [this](const Req& req, Res& res) {
        (void)session(req);
        const json b = body(req);
        schemas_->require("calc-path", b);
        std::shared_lock gate(runner_->gate());
        sendJson(res, 200, calc_->path(b));
    }));

    s.Post(api + "/calc/route", wrap("calc", [this](const Req& req, Res& res) {
        const Principal p = session(req);
        const json mission = body(req);
        schemas_->require("mission", mission);
        std::optional<int> revision;
        const auto stored = missions_->get(p.missionId);
        if (stored && stored->doc == mission) revision = stored->revision;
        json result;
        {
            std::shared_lock gate(runner_->gate());
            result = calc_->route(mission, revision);
        }
        if (revision.has_value() && p.role == Role::Edit) missions_->storeResultIfCurrent(p.missionId, mission, result);
        sendJson(res, 200, result);
    }));

    s.Post(api + "/calc/coverage", wrap("coverage", [this](const Req& req, Res& res) {
        const Principal p = session(req);
        const json mission = body(req);
        schemas_->require("mission", mission);
        const std::size_t cells = Calculator::coverageCells(mission);
        if (cells == 0) throw HttpError(422, "coverage needs coverage.radiusM, coverage.cellM and coverage.droneAglM");
        if (cells > config_.coverage.maxCells) {
            throw HttpError(422, "the grid would have " + std::to_string(cells) + " cells; this server allows " +
                                     std::to_string(config_.coverage.maxCells) + ". Use a larger cellM or a smaller radius.");
        }
        if (jobs_->running() >= static_cast<std::size_t>(config_.coverage.maxJobs) * 4) {
            throw HttpError(503, "too many coverage jobs queued; try again shortly", {{"retryAfterS", 30}});
        }
        sendJson(res, 202, jobs_->start(p.missionId, mission));
    }));

    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+))", wrap("default", [this](const Req& req, Res& res) {
        sendJson(res, 200, jobs_->status(session(req).missionId, req.matches[1]));
    }));
    s.Delete(api + R"(/jobs/([A-Za-z0-9_-]+))", wrap("default", [this](const Req& req, Res& res) {
        jobs_->cancel(session(req).missionId, req.matches[1]);
        res.status = 204;
    }));
    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+)/coverage/(control|video)\.json)", wrap("default", [this](const Req& req, Res& res) {
        sendJson(res, 200, jobs_->grid(session(req).missionId, req.matches[1], req.matches[2]));
    }));

    s.Get(api + "/elevation/profile", wrap("calc", [this](const Req& req, Res& res) {
        if (!req.has_param("from") || !req.has_param("to")) throw HttpError(400, "from and to are required (lat,lon)");
        const auto from = parseLatLon(req.get_param_value("from"));
        const auto to = parseLatLon(req.get_param_value("to"));
        const DemSource& dem = links_->requireDem();
        const auto built = rfcore::ProfileBuilder(dem).build(from, to);
        if (!built.profile) {
            throw HttpError(422, "missing elevation data on part of the path; nothing is guessed",
                            links_->missingData(built.missing, from, to));
        }
        const auto& smp = built.profile->samples();
        sendJson(res, 200, LinkService::profileJson(*built.profile, (smp[1].distance - smp[0].distance).value()));
    }));

    // ---- library ----
    auto kindOf = [this](const std::string& kind) {
        (void)SchemaRegistry::libraryFile(kind);
        if (kind == "radio-profiles" && !builtins_->enabled(BuiltinModules::kRadioProfiles)) {
            throw HttpError(404, "radio profiles (rf.radio-profiles) are disabled on this server");
        }
        return kind;
    };
    s.Get(api + R"(/library/([a-z-]+))", wrap("default", [this, kindOf](const Req& req, Res& res) {
        sendJson(res, 200, library_->list(kindOf(req.matches[1])));
    }));
    s.Get(api + R"(/library/([a-z-]+)/([a-z0-9._-]+))", wrap("default", [this, kindOf](const Req& req, Res& res) {
        const auto item = library_->get(kindOf(req.matches[1]), req.matches[2]);
        if (!item) throw HttpError(404, "no such library item");
        sendJson(res, 200, *item);
    }));
    s.Get(api + R"(/library/([a-z-]+)/([a-z0-9._-]+)/history)", wrap("default", [this, kindOf](const Req& req, Res& res) {
        sendJson(res, 200, library_->history(kindOf(req.matches[1]), req.matches[2]));
    }));
    s.Post(api + R"(/library/([a-z-]+))", wrap("admin", [this, kindOf](const Req& req, Res& res) {
        requireAdmin(req);
        sendJson(res, 201, library_->add(kindOf(req.matches[1]), body(req), "admin@" + client(req)));
    }));
    s.Put(api + R"(/library/([a-z-]+)/([a-z0-9._-]+))", wrap("admin", [this, kindOf](const Req& req, Res& res) {
        requireAdmin(req);
        sendJson(res, 200, library_->update(kindOf(req.matches[1]), req.matches[2], body(req), "admin@" + client(req)));
    }));

    // ---- weather ----
    s.Get(api + "/weather/point", wrap("default", [this](const Req& req, Res& res) {
        const double lat = queryNumber(req, "lat"), lon = queryNumber(req, "lon");
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) throw HttpError(400, "lat/lon out of range");
        TimePoint t = Clock::now();
        if (req.has_param("timeUtc")) {
            const auto parsed = parseIsoUtc(req.get_param_value("timeUtc"));
            if (!parsed) throw HttpError(400, "timeUtc is not ISO 8601");
            t = *parsed;
        }
        const ForecastAt f = weather_->at(lat, lon, t);
        json out = f.point;
        out["notes"] = json::array();
        for (const auto& n : f.notes) out["notes"].push_back({{"what", n.what}, {"detail", n.detail}});
        sendJson(res, 200, out);
    }));
    s.Get(api + "/weather/horizon", wrap("default", [this](const Req&, Res& res) {
        const rfcore::GeoPoint probe = dem_ ? dem_->raster().centre() : rfcore::GeoPoint{0.0, 0.0};
        sendJson(res, 200, weather_->horizon(probe.latitudeDeg, probe.longitudeDeg));
    }));

    s.Get(api + "/config/map", wrap("default", [this](const Req&, Res& res) { sendJson(res, 200, mapConfig()); }));

    // ---- modules ----
    s.Get(api + "/modules", wrap("default", [this](const Req& req, Res& res) { sendJson(res, 200, moduleList(isAdmin(req))); }));

    s.Get(api + R"(/modules/([a-z0-9._-]+)/(.+))", wrap("default", [this](const Req& req, Res& res) {
        const std::string id = req.matches[1];
        const std::string path = req.matches[2];
        std::optional<std::filesystem::path> file;
        bool alpha = false;
        {
            std::shared_lock gate(runner_->gate());
            // publicFile serves ui/ and the options schema only, never lib/.
            file = registry_->publicFile(id, path);
            if (const auto* m = registry_->instance(id)) alpha = m->manifest().lifecycle == rfmod::Lifecycle::Alpha;
        }
        if (!file) throw HttpError(404, "no such module file");
        if (alpha && !isAdmin(req) && !(req.has_param("grant") && grantValid(id, req.get_param_value("grant")))) {
            throw HttpError(404, "no such module file");
        }
        std::ifstream in(*file, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        const std::string ext = file->extension().string();
        const char* type = ext == ".js" || ext == ".mjs" ? "text/javascript"
                           : ext == ".json"              ? "application/json"
                           : ext == ".css"               ? "text/css"
                           : ext == ".svg"               ? "image/svg+xml"
                           : ext == ".png"               ? "image/png"
                                                         : "application/octet-stream";
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Cache-Control", alpha ? "private, no-store" : "public, max-age=300");
        res.status = 200;
        res.set_content(content.str(), type);
    }));

    // ---- admin ----
    s.Post(api + "/admin/modules", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        const bool install = !req.has_param("install") || req.get_param_value("install") != "false";
        rfmod::ValidationReport report;
        {
            std::unique_lock gate(runner_->gate());
            report = registry_->upload(req.body, install);
        }
        log_.info("modules", std::string("upload ") + (report.ok() ? "passed" : "refused") + (install ? ", install" : ", check only"));
        sendJson(res, 200, report.toJson());
    }));
    s.Put(api + R"(/admin/modules/([a-z0-9._-]+))", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        const std::string id = req.matches[1];
        const json b = body(req);
        std::optional<bool> enabled;
        if (b.contains("enabled")) enabled = b.at("enabled").get<bool>();
        std::optional<json> options;
        if (b.contains("options")) options = b.at("options");
        if (builtins_->isBuiltin(id)) {
            builtins_->set(id, enabled, options);
        } else {
            std::unique_lock gate(runner_->gate());
            try {
                if (options) registry_->setOptions(id, *options);
                if (enabled.has_value()) registry_->setEnabled(id, *enabled);
            } catch (const std::invalid_argument& e) {
                throw HttpError(422, e.what());
            }
        }
        sendJson(res, 200, moduleList(true));
    }));
    s.Delete(api + R"(/admin/modules/([a-z0-9._-]+))", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        if (builtins_->isBuiltin(req.matches[1])) throw HttpError(422, "built-in modules can be disabled, not removed");
        std::unique_lock gate(runner_->gate());
        try {
            registry_->remove(req.matches[1]);
        } catch (const std::invalid_argument& e) {
            throw HttpError(422, e.what());
        }
        res.status = 204;
    }));
    s.Get(api + "/admin/modules/notices", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        std::shared_lock gate(runner_->gate());
        sendJson(res, 200, registry_->notices());
    }));

    auto keyList = [this] {
        json out = json::array();
        for (const auto& k : trust_->keys()) {
            out.push_back({{"id", k.key.id()}, {"label", k.label}, {"key", k.key.toText()}, {"builtIn", k.builtIn},
                           {"firstParty", k.firstParty()}});
        }
        return out;
    };
    s.Get(api + "/admin/keys", wrap("admin", [this, keyList](const Req& req, Res& res) {
        requireAdmin(req);
        std::shared_lock gate(runner_->gate());
        sendJson(res, 200, keyList());
    }));
    s.Post(api + "/admin/keys", wrap("admin", [this, keyList](const Req& req, Res& res) {
        requireAdmin(req);
        const json b = body(req);
        std::unique_lock gate(runner_->gate());
        try {
            trust_->add(rfmod::PublicKey::fromText(b.at("key").get<std::string>()), b.value("label", ""));
        } catch (const std::invalid_argument& e) {
            throw HttpError(422, e.what());
        }
        registry_->reload();
        sendJson(res, 201, keyList());
    }));
    s.Delete(api + R"(/admin/keys/([0-9a-f]+))", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        std::unique_lock gate(runner_->gate());
        try {
            trust_->remove(req.matches[1]);
        } catch (const std::invalid_argument& e) {
            throw HttpError(409, e.what());
        }
        // Modules signed with the removed key stop loading now, not only at the next start.
        registry_->reload();
        res.status = 204;
    }));

    s.Get(api + "/admin/cache", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        sendJson(res, 200, cacheTiers());
    }));
    s.Put(api + "/admin/cache", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        const json b = body(req);
        if (!b.is_array()) throw HttpError(422, "send [{id, limitBytes?, limitRows?}]");
        ResultCache::Limits l = results_->limits();
        std::size_t demLimit = blocks_->stats().limitBytes;
        for (const json& t : b) {
            const std::string id = t.at("id");
            if (id == "dem-blocks") demLimit = t.value("limitBytes", demLimit);
            else if (id == "results-memory") l.memoryBytes = t.value("limitBytes", l.memoryBytes);
            else if (id == "results-table") {
                l.tableBytes = t.value("limitBytes", l.tableBytes);
                l.tableRows = t.value("limitRows", l.tableRows);
            } else throw HttpError(422, "unknown cache tier " + id);
        }
        blocks_->setLimit(demLimit);
        results_->setLimits(l);
        const json stored = {{"demBlocksBytes", demLimit}, {"resultMemoryBytes", l.memoryBytes},
                             {"resultTableRows", l.tableRows}, {"resultTableBytes", l.tableBytes}};
        db_->tx([&](pqxx::work& w) {
            w.exec_params("insert into settings(key, value) values ('cache.limits', $1::jsonb) "
                          "on conflict (key) do update set value = excluded.value", stored.dump());
        });
        sendJson(res, 200, cacheTiers());
    }));
    s.Delete(api + "/admin/cache", wrap("admin", [this](const Req& req, Res& res) {
        requireAdmin(req);
        const std::string tier = req.has_param("tier") ? req.get_param_value("tier") : "all";
        if (tier == "dem-blocks" || tier == "all") blocks_->clear();
        if (tier == "results-memory" || tier == "all") results_->clearMemory();
        if (tier == "results-table" || tier == "all") results_->clearTable();
        if (tier != "all" && tier != "dem-blocks" && tier != "results-memory" && tier != "results-table") {
            throw HttpError(422, "unknown cache tier " + tier);
        }
        res.status = 204;
    }));
}

}  // namespace rfmap
