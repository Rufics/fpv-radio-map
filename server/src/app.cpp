#include "app.hpp"

#include <httplib.h>

#include <chrono>
#include <format>

namespace rfmap {

namespace {

using Req = httplib::Request;
using Res = httplib::Response;

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

void sendJson(Res& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

}  // namespace

Application::Application(Config config, const std::string& ownerKeyText, bool ownerKeyIsDev,
                         const std::filesystem::path& sqlDir)
    : config_(std::move(config)),
      services_(config_, log_, ownerKeyText, ownerKeyIsDev, sqlDir),
      admin_(config_.adminToken),
      missionApi_(services_),
      flightLogApi_(services_),
      dataApi_(services_, admin_),
      adminApi_(services_, admin_),
      startTimeSeconds_(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()),
      http_(std::make_unique<httplib::Server>()) {
    configureServer();
    routes();
    ops_ = makeOpsServer();
    stopper_ = std::make_unique<GracefulStop>(
        std::chrono::seconds(config_.ops.drainSeconds),
        [this] {
            log_.info("http", std::format("stop requested: not ready, draining for {} s", config_.ops.drainSeconds));
            ops_->setDraining(true);
        },
        [this] { stop(); });
}

Application::~Application() {
    stopper_.reset();
    ops_->stop();
    if (http_) http_->stop();
}

std::unique_ptr<OpsServer> Application::makeOpsServer() {
    auto ops = std::make_unique<OpsServer>(config_.ops, MetricsAccess::fromConfig(config_.metrics),
                                           config_.metrics.enabled, log_);
    dbProbe_ = std::make_unique<DatabaseProbe>(config_.database.url, std::chrono::milliseconds(1000), log_);
    ops->readiness().addRequired("database", [this] { return dbProbe_->check(); });
    ops->readiness().addInformational("dem", [this] {
        const bool dem = services_.dem() != nullptr;
        return CheckResult{dem, dem ? "" : "no DEM configured: calculations answer 503"};
    });
    ops->readiness().addInformational("weather", [this] {
        const bool weather = services_.weather().configured();
        return CheckResult{weather, weather ? "" : "no forecasts"};
    });
    ops->setMetricsSource([this] { return metricsText(); });
    return ops;
}

bool Application::listen() {
    if (config_.ops.enabled && !ops_->listen()) return false;
    log_.info("http", std::format("listening on {}:{}", config_.server.listen, config_.server.port));
    const bool ok = http_->listen(config_.server.listen, config_.server.port);
    if (!ok) log_.error("http", std::format("could not listen on {}:{}", config_.server.listen, config_.server.port));
    ops_->stop();
    return ok;
}

int Application::bindAny() { return http_->bind_to_any_port(config_.server.listen); }
int Application::bindAnyOps() { return ops_->bindAny(); }
void Application::serveBound() { http_->listen_after_bind(); }
void Application::stop() { http_->stop(); }

std::string Application::metricsText() const {
    ServerSample sample;
    sample.version = RFMAP_HOST_VERSION;
    sample.startTimeSeconds = startTimeSeconds_;
    sample.process = readProcessStats();
    sample.jobs = services_.jobs().stats();
    sample.pools = {{"http", httpMetrics_.pool(config_.server.threads)},
                    {"job-runners", sample.jobs.runners},
                    {"job-cells", sample.jobs.cells}};
    sample.db = services_.db().stats();
    sample.caches = {{"dem-blocks", services_.blocks().stats()},
                     {"results-memory", services_.results().memoryStats()},
                     {"results-table", services_.results().tableCounts(), false}};
    PrometheusText out;
    httpMetrics_.write(out);
    writeServerMetrics(out, sample);
    return out.text();
}

template <typename Fn>
void Application::dispatch(const std::string& rateClass, const Fn& handler, const Req& req, Res& res) {
    const HttpRequest request(req, config_.server);
    HttpReply reply(res);
    try {
        services_.limiter().take(rateClass, request.client());
        handler(request, reply);
    } catch (const HttpError& e) {
        reply.error(e);
    } catch (const json::exception& e) {
        reply.send(400, {{"message", std::string("malformed request: ") + e.what()}});
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): HTTP boundary, any other failure is a logged 500
        internalError(req, res, e.what());
    }
    // A non-std exception reaches the server's exception handler set in configureServer().
}

void Application::internalError(const Req& req, Res& res, std::string_view what) {
    log_.error("http", std::format("{} {}: {}", req.method, HttpRequest::redacted(req.path), what));
    sendJson(res, 500, {{"message", "internal error"}});
}

Application::RawHandler Application::route(std::string rateClass, Handler handler) {
    return [this, rateClass = std::move(rateClass), handler = std::move(handler)](const Req& req, Res& res) {
        const auto started = std::chrono::steady_clock::now();
        dispatch(rateClass, handler, req, res);
        httpMetrics_.record(rateClass, req.method, res.status,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    };
}

void Application::configureServer() {
    httplib::Server& s = *http_;
    const int threads = config_.server.threads;
    // httplib takes ownership of the queue it gets and deletes it when the server stops.
    s.new_task_queue = [this, threads] {
        return std::make_unique<CountingTaskQueue>(static_cast<std::size_t>(threads), httpMetrics_).release();
    };
    s.set_payload_max_length(config_.server.maxUploadBytes);
    // Runs before httplib reads the body: a JSON request announcing more than maxBodyBytes is
    // answered at once instead of after up to maxUploadBytes have been buffered for it.
    // A JSON body sent chunked (no length to judge) is refused too: httplib would bound it only
    // by maxUploadBytes. A compressed body is refused on every route: the limits count the
    // bytes received, and httplib would inflate it while reading.
    s.set_pre_routing_handler([limit = BodyLimit(config_.server)](const Req& req, Res& res) {
        using enum httplib::Server::HandlerResponse;
        if (BodyLimit::encodingRefused(req.get_header_value("Content-Encoding"))) {
            sendJson(res, 415, {{"message", "compressed request bodies are not accepted"}});
            return Handled;
        }
        if (limit.lengthRequired(req.path, req.get_header_value("Transfer-Encoding"))) {
            sendJson(res, 411, {{"message", "send JSON bodies with a Content-Length"}});
            return Handled;
        }
        if (!req.has_header("Content-Length") || !limit.exceeded(req.path, req.get_header_value("Content-Length"))) {
            return Unhandled;
        }
        sendJson(res, 413, {{"message", "request body too large"}});
        return Handled;
    });
    s.set_read_timeout(30, 0);
    s.set_write_timeout(120, 0);
    s.set_logger([this](const Req& req, const Res& res) {
        log_.info("http", std::format("{} {} {}", req.method, HttpRequest::redacted(req.path), res.status));
    });
    // Errors dispatch does not answer itself; without a handler httplib would put what() in a header.
    s.set_exception_handler([this](const Req& req, Res& res, const std::exception_ptr&) {
        internalError(req, res, "unexpected exception");
    });
    s.set_error_handler([](const Req&, Res& res) {
        if (res.body.empty()) sendJson(res, res.status, {{"message", httplib::status_message(res.status)}});
    });
}

void Application::routes() {
    const std::string api = "/api/v1";
    missionRoutes(api);
    flightLogRoutes(api);
    dataRoutes(api);
    adminRoutes(api);
}

void Application::missionRoutes(const std::string& api) {
    httplib::Server& s = *http_;
    const MissionApi& m = missionApi_;
    const std::string mission = api + R"(/missions/([A-Za-z0-9_-]+))";
    s.Get(api + "/health", route("default", on(m, &MissionApi::getHealth)));

    s.Post(api + "/missions", route("create", on(m, &MissionApi::postMission)));
    s.Get(mission, route("default", on(m, &MissionApi::getMission)));
    s.Put(mission, route("default", on(m, &MissionApi::putMission)));

    // Links are managed with the owner key; opening one is public, before the PIN.
    s.Get(mission + "/links", route("default", on(m, &MissionApi::getLinks)));
    s.Post(mission + "/links", route("default", on(m, &MissionApi::postLink)));
    s.Delete(mission + R"(/links/([A-Za-z0-9_-]+))", route("default", on(m, &MissionApi::deleteLink)));
    s.Get(api + R"(/links/([A-Za-z0-9_-]+))", route("open", on(m, &MissionApi::getLink)));
    s.Post(api + R"(/links/([A-Za-z0-9_-]+)/open)", route("open", on(m, &MissionApi::postLinkOpen)));

    s.Post(api + "/calc/path", route("calc", on(m, &MissionApi::postCalcPath)));
    s.Post(api + "/calc/route", route("calc", on(m, &MissionApi::postCalcRoute)));
    s.Post(api + "/calc/coverage", route("coverage", on(m, &MissionApi::postCalcCoverage)));
    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+))", route("default", on(m, &MissionApi::getJob)));
    s.Delete(api + R"(/jobs/([A-Za-z0-9_-]+))", route("default", on(m, &MissionApi::deleteJob)));
    s.Get(api + R"(/jobs/([A-Za-z0-9_-]+)/coverage/(control|video)\.json)", route("default", on(m, &MissionApi::getJobGrid)));
    s.Get(api + "/elevation/profile", route("calc", on(m, &MissionApi::getElevationProfile)));
}

void Application::flightLogRoutes(const std::string& api) {
    httplib::Server& s = *http_;
    const FlightLogApi& f = flightLogApi_;
    const std::string flightLogs = api + R"(/missions/([A-Za-z0-9_-]+)/flightlogs)";
    s.Post(api + "/flightlogs/decode", route("calc", on(f, &FlightLogApi::postDecode)));
    s.Get(flightLogs, route("default", on(f, &FlightLogApi::getList)));
    s.Post(flightLogs, route("calc", on(f, &FlightLogApi::postImport)));
    s.Get(flightLogs + R"(/([A-Za-z0-9_-]+))", route("default", on(f, &FlightLogApi::getFlight)));
    s.Delete(flightLogs + R"(/([A-Za-z0-9_-]+))", route("default", on(f, &FlightLogApi::deleteFlight)));
}

void Application::dataRoutes(const std::string& api) {
    httplib::Server& s = *http_;
    const DataApi& d = dataApi_;
    const std::string library = api + R"(/library/([a-z-]+))";
    const std::string libraryItem = library + R"(/([a-z0-9._-]+))";
    s.Get(library, route("default", on(d, &DataApi::getLibraryList)));
    s.Get(libraryItem, route("default", on(d, &DataApi::getLibraryItem)));
    s.Get(libraryItem + "/history", route("default", on(d, &DataApi::getLibraryHistory)));
    s.Post(library, route("admin", on(d, &DataApi::postLibraryItem)));
    s.Put(libraryItem, route("admin", on(d, &DataApi::putLibraryItem)));

    s.Get(api + "/weather/point", route("default", on(d, &DataApi::getWeatherPoint)));
    s.Get(api + "/weather/horizon", route("default", on(d, &DataApi::getWeatherHorizon)));
    s.Get(api + "/config/map", route("default", on(d, &DataApi::getMapConfig)));

    s.Get(api + "/modules", route("default", on(d, &DataApi::getModules)));
    s.Get(api + R"(/modules/([a-z0-9._-]+)/(.+))", route("default", on(d, &DataApi::getModuleFile)));
}

void Application::adminRoutes(const std::string& api) {
    httplib::Server& s = *http_;
    const AdminApi& a = adminApi_;
    // Modules are installed and keys trusted on the machine (rfmod CLI); the API reads and reloads.
    s.Post(api + "/admin/modules/reload", route("admin", on(a, &AdminApi::postModulesReload)));
    s.Get(api + "/admin/modules/notices", route("admin", on(a, &AdminApi::getNotices)));
    s.Put(api + R"(/admin/modules/([a-z0-9._-]+))", route("admin", on(a, &AdminApi::putModule)));
    s.Get(api + "/admin/keys", route("admin", on(a, &AdminApi::getKeys)));
    s.Get(api + "/admin/cache", route("admin", on(a, &AdminApi::getCache)));
    s.Put(api + "/admin/cache", route("admin", on(a, &AdminApi::putCache)));
    s.Delete(api + "/admin/cache", route("admin", on(a, &AdminApi::deleteCache)));
    // Metrics are on the operations listener (ops_server.hpp), not behind the admin token.
}

}  // namespace rfmap
