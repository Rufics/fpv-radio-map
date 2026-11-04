// The tool end to end against a stand-in server: what it sends, what it prints, how it exits.

#include "cli.hpp"
#include "service_token.hpp"

#include <httplib.h>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace rfmap::cli;
namespace fs = std::filesystem;

namespace {

/// An httplib server on a free loopback port that records every request it answers.
class FakeServer {
public:
    struct Seen {
        std::string method;
        std::string path;  ///< with the query
        std::string body;
        std::string authorization;
    };

    FakeServer() {
        // Recorded before the handler answers, so a request is listed by the time its client
        // has the answer (a logger runs after the response is sent).
        server_.set_pre_request_handler([this](const httplib::Request& req, httplib::Response&) {
            const std::scoped_lock lock(mutex_);
            seen_.push_back({req.method, req.target, req.body, req.get_header_value("Authorization")});
            return httplib::Server::HandlerResponse::Unhandled;
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~FakeServer() {
        server_.stop();
        thread_.join();
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;
    FakeServer(FakeServer&&) = delete;
    FakeServer& operator=(FakeServer&&) = delete;

    httplib::Server& http() { return server_; }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    [[nodiscard]] std::vector<Seen> seen() const {
        const std::scoped_lock lock(mutex_);
        return seen_;
    }

    static void reply(httplib::Response& res, int status, const json& body) {
        res.status = status;
        res.set_content(body.dump(), "application/json");
    }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<Seen> seen_;
};

/// One run of the tool: its exit status, stdout, stderr and the waits it asked for.
struct Outcome {
    int status;
    std::string out;
    std::string err;
    std::vector<std::chrono::milliseconds> sleeps;
    [[nodiscard]] json result() const { return json::parse(out); }
};

Outcome run(std::vector<std::string> words, std::map<std::string, std::string, std::less<>> env = {},
        const std::string& stdinText = "") {
    std::istringstream in(stdinText);
    std::ostringstream out;
    std::ostringstream err;
    auto sleeps = std::make_shared<std::vector<std::chrono::milliseconds>>();
    Cli cli("9.8.7", {in, out, err, std::move(env), [sleeps](std::chrono::milliseconds ms) { sleeps->push_back(ms); }});
    const int status = cli.run(words);
    return {status, out.str(), err.str(), *sleeps};
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("rfmap-cli-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir / name;
}

void writeFile(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

json mission() {
    return {{"schema", "mission/v1"},
            {"name", "CI"},
            {"pilot", {{"lat", 48.5}, {"lon", 35.2}, {"aglM", 1.5}}},
            {"waypoints", {{{"lat", 48.52}, {"lon", 35.22}, {"aglM", 120}}}}};
}

json routeResult(const char* status, double margin) {
    return {{"legs",
             {{{"index", 0},
               {"from", "Pilot"},
               {"to", "WP1"},
               {"lengthM", 3000.0},
               {"status", status},
               {"worst", {{"control", {{"kind", "control"}, {"marginDb", margin}}}}}}}}};
}

}  // namespace

TEST(Cli, HelpAndVersionNeedNoServer) {
    const Outcome help = run({"--help"});
    EXPECT_EQ(help.status, Cli::kOk);
    EXPECT_NE(help.out.find("calc route MISSION.json"), std::string::npos);
    const Outcome version = run({"--version"});
    EXPECT_EQ(version.out, "rfmap 9.8.7\n");
    const Outcome local = run({"version"});
    EXPECT_EQ(local.result(), json({{"client", "9.8.7"}}));
}

TEST(Cli, NoCommandOrNoServerIsAUsageError) {
    EXPECT_EQ(run({}).status, Cli::kUsage);
    const Outcome r = run({"health"});
    EXPECT_EQ(r.status, Cli::kUsage);
    EXPECT_NE(r.err.find("no server"), std::string::npos) << r.err;
    EXPECT_EQ(run({"--server", "http://x", "nonsense"}).status, Cli::kUsage);
    EXPECT_EQ(run({"--server", "http://x", "health", "--min-margn", "3"}).status, Cli::kUsage);
}

TEST(Cli, HealthPrintsTheServersAnswerAndExitsOneUnlessOk) {
    FakeServer s;
    bool ok = true;
    s.http().Get("/api/v1/health", [&ok](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"ok", ok}, {"version", "1.2.3"}});
    });
    const Outcome up = run({"health", "--server", s.url()});
    EXPECT_EQ(up.status, Cli::kOk);
    EXPECT_EQ(up.result()["version"], "1.2.3");
    ok = false;
    EXPECT_EQ(run({"health"}, {{"RFMAP_SERVER", s.url()}}).status, Cli::kCheckFailed);
    EXPECT_EQ(run({"version", "--server", s.url()}).result(), json({{"client", "9.8.7"}, {"server", "1.2.3"}}));
}

TEST(Cli, CalcRouteSendsTheMissionWithTheTokenAndChecksTheResult) {
    FakeServer s;
    json answer = routeResult("blocked", -4.0);
    s.http().Post("/api/v1/calc/route", [&answer](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, answer);
    });
    const fs::path file = scratch("mission.json");
    writeFile(file, mission().dump());
    const std::map<std::string, std::string, std::less<>> env{{"RFMAP_SERVER", s.url()}, {"RFMAP_TOKEN", "svc-token"}};

    const Outcome blocked = run({"calc", "route", file.string(), "--fail-on", "blocked", "--summary"}, env);
    EXPECT_EQ(blocked.status, Cli::kCheckFailed);
    EXPECT_EQ(blocked.result(), answer);  // the result is printed even when a check fails
    EXPECT_NE(blocked.err.find("check failed: leg 1 (Pilot -> WP1) is blocked"), std::string::npos) << blocked.err;
    EXPECT_NE(blocked.err.find("control -4.0 dB"), std::string::npos) << blocked.err;
    ASSERT_EQ(s.seen().size(), 1U);
    EXPECT_EQ(s.seen()[0].authorization, "Bearer svc-token");
    EXPECT_EQ(json::parse(s.seen()[0].body), mission());

    answer = routeResult("ok", 12.0);
    EXPECT_EQ(run({"calc", "route", file.string(), "--fail-on", "warn", "--min-margin", "10"}, env).status, Cli::kOk);
    EXPECT_EQ(run({"calc", "route", file.string(), "--min-margin", "15"}, env).status, Cli::kCheckFailed);
    EXPECT_EQ(run({"calc", "route", file.string(), "--fail-on", "maybe"}, env).status, Cli::kUsage);
    // The mission can come from stdin.
    EXPECT_EQ(run({"calc", "route", "-"}, env, mission().dump()).status, Cli::kOk);
    EXPECT_EQ(json::parse(s.seen().back().body), mission());
}

TEST(Cli, CalcPathBuildsTheLinkFromFlags) {
    FakeServer s;
    s.http().Post("/api/v1/calc/path", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"kind", "video"}, {"marginDb", 7.5}, {"reachable", true}});
    });
    const Outcome r = run({"--server", s.url(), "calc", "path", "--from", "48.5,35.2,1.5", "--to", "48.52,35.22,120",
                       "--kind", "video", "--radio", "dji-o4", "--tx-power", "27", "--ground-antenna", "patch",
                       "--air-antenna", "dipole", "--required-margin", "6", "--time", "2026-11-04T10:00:00Z",
                       "--min-margin", "5"});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    const json sent = json::parse(s.seen().at(0).body);
    EXPECT_EQ(sent["from"], json({{"lat", 48.5}, {"lon", 35.2}, {"aglM", 1.5}}));
    EXPECT_EQ(sent["kind"], "video");
    EXPECT_EQ(sent["link"], json({{"radio", "dji-o4"},
                                  {"txPowerDbm", 27.0},
                                  {"groundAntenna", "patch"},
                                  {"airAntenna", "dipole"},
                                  {"requiredMarginDb", 6.0}}));
    EXPECT_EQ(sent["timeUtc"], "2026-11-04T10:00:00Z");

    // Both links from a file; the margin check sees the result's margin.
    const fs::path links = scratch("links.json");
    writeFile(links, R"({"control": {"radio": "elrs"}, "video": {"radio": "dji"}})");
    EXPECT_EQ(run({"--server", s.url(), "calc", "path", "--from", "48.5,35.2,1.5", "--to", "48.52,35.22,120", "--links",
                   links.string(), "--evaluate", "both", "--min-margin", "8"})
                  .status,
              Cli::kCheckFailed);
    EXPECT_EQ(json::parse(s.seen().back().body)["evaluate"], "both");
    EXPECT_EQ(run({"--server", s.url(), "calc", "path", "--from", "48.5,35.2,1.5", "--to", "48.52,35.22,120", "--radio",
                   "x"})
                  .status,
              Cli::kUsage);  // --tx-power and the rest are missing
}

TEST(Cli, AServerErrorExitsTwoWithTheMessageAndTheSchemaIssues) {
    FakeServer s;
    s.http().Post("/api/v1/calc/route", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 422, {{"message", "the mission does not match its schema"},
                                     {"issues", {{{"path", "/pilot/lat"}, {"message", "must be a number"}}}}});
    });
    const Outcome r = run({"--server", s.url(), "calc", "route", "-"}, {}, "{}");
    EXPECT_EQ(r.status, Cli::kServerError);
    EXPECT_NE(r.err.find("422 Unprocessable Content: the mission does not match its schema"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("/pilot/lat: must be a number"), std::string::npos) << r.err;
    EXPECT_TRUE(r.out.empty());
}

TEST(Cli, BusyAnswersAreRetriedAfterTheTimeTheServerAsksFor) {
    FakeServer s;
    std::atomic<int> calls = 0;
    s.http().Get("/api/v1/modules", [&calls](const httplib::Request&, httplib::Response& res) {
        if (calls++ < 2) {
            FakeServer::reply(res, 429, {{"message", "slow down"}, {"retryAfterS", 7}});
            return;
        }
        FakeServer::reply(res, 200, json::array({{{"id", "fpv.core"}}}));
    });
    const Outcome r = run({"--server", s.url(), "modules"});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    EXPECT_EQ(r.sleeps, (std::vector<std::chrono::milliseconds>{std::chrono::seconds(7), std::chrono::seconds(7)}));
    calls = 0;
    const Outcome tooFew = run({"--server", s.url(), "--retries", "1", "modules"});
    EXPECT_EQ(tooFew.status, Cli::kServerError);
    EXPECT_NE(tooFew.err.find("429"), std::string::npos);
}

TEST(Cli, AnUnreachableServerExitsThreeAfterRetryingReads) {
    std::string url;
    {
        const FakeServer gone;
        url = gone.url();
    }  // the port is free again: nothing listens there
    const Outcome r = run({"--server", url, "--retries", "2", "map"});
    EXPECT_EQ(r.status, Cli::kUnreachable);
    EXPECT_EQ(r.sleeps.size(), 2U);
    EXPECT_NE(r.err.find("cannot reach the server"), std::string::npos);
    // A calculation is not repeated after a failed connection: it may have run.
    EXPECT_TRUE(run({"--server", url, "calc", "route", "-"}, {}, "{}").sleeps.empty());
}

TEST(Cli, OnlyAServerOnThisMachineMaySkipHttps) {
    EXPECT_EQ(ApiClient::splitServer("https://maps.example.org/maps/"),
              (std::pair<std::string, std::string>{"https://maps.example.org", "/maps"}));
    EXPECT_EQ(ApiClient::splitServer("http://127.0.0.1:8080").first, "http://127.0.0.1:8080");
    for (const char* local : {"http://localhost", "http://LocalHost:8080/x", "http://127.4.5.6", "http://[::1]:8080"}) {
        EXPECT_NO_THROW((void)ApiClient::splitServer(local)) << local;
    }
    for (const char* refused :
         {"http://maps.example.org", "http://10.0.0.5:8080", "http://[::2]", "http://127.0.0.1.example.org",
          "http://localhost.example.org", "http://127.0.0.1@maps.example.org", "https://user:pw@maps.example.org",
          "ftp://x", "maps.example.org", "https://", "https:///maps", "https://:8080"}) {
        EXPECT_THROW((void)ApiClient::splitServer(refused), ServerUrlError) << refused;
    }
    // A wrong server URL is a wrong argument, wherever it comes from, and nothing is sent.
    const Outcome r = run({"--server", "http://maps.example.org", "--token", "t", "map"});
    EXPECT_EQ(r.status, Cli::kUsage);
    EXPECT_NE(r.err.find("https://"), std::string::npos);
    EXPECT_EQ(run({"map"}, {{"RFMAP_SERVER", "ftp://x"}}).status, Cli::kUsage);
}

TEST(Cli, ServerUrlsMayHaveAPathPrefixAndTokensComeFromFiles) {
    FakeServer s;
    s.http().Get("/maps/api/v1/config/map", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"zoom", 11}});
    });
    const fs::path token = scratch("token");
    writeFile(token, "file-token\n");
    const Outcome r = run({"--server", s.url() + "/maps/", "--token-file", token.string(), "map", "--compact"});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    EXPECT_EQ(r.out, "{\"zoom\":11}\n");
    EXPECT_EQ(s.seen().at(0).authorization, "Bearer file-token");
    EXPECT_EQ(run({"--server", s.url(), "--token-file", "/no/such/file", "map"}).status, Cli::kNoInput);
}

TEST(Cli, ReadCommandsAskTheRightPaths) {
    FakeServer s;
    s.http().Get(R"(/api/v1/.*)", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, json::object());
    });
    const std::string server = "--server=" + s.url();
    for (const std::vector<std::string>& words :
         {std::vector<std::string>{server, "library", "list", "antennas"},
          {server, "library", "get", "radio-profiles", "elrs-915-50hz"},
          {server, "library", "history", "antennas", "dipole.v2"},
          {server, "weather", "point", "--lat", "48.5", "--lon", "35.2", "--time", "2026-11-04T10:00:00+02:00"},
          {server, "weather", "horizon"},
          {server, "elevation", "--from", "48.5,35.2", "--to", "48.52,35.22"},
          {server, "regions", "list"},
          {server, "regions", "get", "ukraine-east"},
          {server, "api", "get", "/admin/cache"}}) {
        EXPECT_EQ(run(words).status, Cli::kOk) << words[1];
    }
    std::vector<std::string> paths;
    for (const auto& seen : s.seen()) paths.push_back(seen.path);
    EXPECT_EQ(paths, (std::vector<std::string>{
                         "/api/v1/library/antennas",
                         "/api/v1/library/radio-profiles/elrs-915-50hz",
                         "/api/v1/library/antennas/dipole.v2/history",
                         // httplib sends ":" and "," as they are; the "+" of the offset stays encoded, or it would read as a space.
                         "/api/v1/weather/point?lat=48.5&lon=35.2&timeUtc=2026-11-04T10:00:00%2B02:00",
                         "/api/v1/weather/horizon",
                         "/api/v1/elevation/profile?from=48.5,35.2&to=48.52,35.22",
                         "/api/v1/regions",
                         "/api/v1/regions/ukraine-east",
                         "/api/v1/admin/cache",
                     }));
    // Nothing a user types can leave the path it goes into.
    EXPECT_EQ(run({server, "library", "get", "antennas", "../../admin/keys"}).status, Cli::kUsage);
    EXPECT_EQ(run({server, "regions", "get", "Ukraine"}).status, Cli::kUsage);
    EXPECT_EQ(run({server, "api", "GET", "admin/cache"}).status, Cli::kUsage);
    EXPECT_EQ(run({server, "api", "PATCH", "/x"}).status, Cli::kUsage);
}

TEST(Cli, ApiSendsAnyCallAndOutputWritesTheResultToAFile) {
    FakeServer s;
    s.http().Put("/api/v1/admin/cache", [](const httplib::Request& req, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"echo", json::parse(req.body)}});
    });
    s.http().Delete("/api/v1/admin/cache", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    const fs::path out = scratch("out.json");
    const Outcome put = run({"--server", s.url(), "api", "PUT", "/admin/cache", "--data", "-", "--output", out.string()}, {},
                        R"({"limits": {"results-memory": 1}})");
    EXPECT_EQ(put.status, Cli::kOk) << put.err;
    EXPECT_TRUE(put.out.empty());
    std::ifstream in(out);
    EXPECT_EQ(json::parse(in)["echo"]["limits"]["results-memory"], 1);
    const Outcome del = run({"--server", s.url(), "api", "delete", "/admin/cache"});
    EXPECT_EQ(del.status, Cli::kOk) << del.err;
    EXPECT_EQ(del.out, "null\n");
    EXPECT_EQ(run({"--server", s.url(), "api", "PUT", "/admin/cache", "--data", "-"}, {}, "{not json").status,
              Cli::kNoInput);
    EXPECT_EQ(run({"--server", s.url(), "api", "PUT", "/admin/cache", "--output", "/no/such/dir/out.json"}).status,
              Cli::kCantCreate);
}

TEST(Cli, CoverageWaitsForTheJobAndFetchesEveryGrid) {
    FakeServer s;
    std::atomic<int> polls = 0;
    s.http().Post("/api/v1/calc/coverage", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 202, {{"jobId", "job-1"}, {"state", "queued"}, {"parts", json::array()}});
    });
    s.http().Get("/api/v1/jobs/job-1", [&polls](const httplib::Request&, httplib::Response& res) {
        const bool done = ++polls >= 3;
        FakeServer::reply(res, 200,
                          {{"jobId", "job-1"},
                           {"state", done ? "done" : "running"},
                           {"parts", {{{"link", "control"}, {"state", done ? "done" : "running"}, {"progress", done ? 1.0 : 0.5}},
                                      {{"link", "video"}, {"state", done ? "cached" : "running"}, {"progress", 1.0}}}}});
    });
    s.http().Get(R"(/api/v1/jobs/job-1/coverage/(control|video)\.json)",
                 [](const httplib::Request& req, httplib::Response& res) {
                     FakeServer::reply(res, 200, {{"link", req.matches[1].str()}, {"cols", 3}});
                 });
    const std::string server = "--server=" + s.url();
    const Outcome r = run({server, "calc", "coverage", "-", "--poll", "0.5"}, {}, mission().dump());
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    EXPECT_EQ(r.result()["grids"]["video"]["link"], "video");
    EXPECT_EQ(r.result()["job"]["state"], "done");
    EXPECT_EQ(r.sleeps, (std::vector<std::chrono::milliseconds>(3, std::chrono::milliseconds(500))));
    EXPECT_NE(r.err.find("coverage job-1: control 50%, video 100%"), std::string::npos) << r.err;

    polls = 0;
    const fs::path dir = scratch("grids");
    const Outcome files = run({server, "calc", "coverage", "-", "--out-dir", dir.string()}, {}, mission().dump());
    EXPECT_EQ(files.status, Cli::kOk) << files.err;
    EXPECT_EQ(files.result()["files"]["control"], (dir / "control.json").string());
    std::ifstream grid(dir / "control.json");
    EXPECT_EQ(json::parse(grid)["cols"], 3);

    const Outcome started = run({server, "calc", "coverage", "-", "--no-wait"}, {}, mission().dump());
    EXPECT_EQ(started.result()["jobId"], "job-1");
    EXPECT_TRUE(started.sleeps.empty());
}

TEST(Cli, ACoverageJobThatOutlastsTheWaitIsCancelled) {
    FakeServer s;
    s.http().Post("/api/v1/calc/coverage", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 202, {{"jobId", "slow"}, {"state", "running"}, {"parts", json::array()}});
    });
    s.http().Get("/api/v1/jobs/slow", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"jobId", "slow"}, {"state", "running"}, {"parts", json::array()}});
    });
    s.http().Delete("/api/v1/jobs/slow", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    const Outcome r = run({"--server", s.url(), "calc", "coverage", "-", "--poll", "10", "--wait", "25"}, {}, "{}");
    EXPECT_EQ(r.status, Cli::kCheckFailed);
    EXPECT_EQ(r.sleeps.size(), 3U);
    EXPECT_EQ(s.seen().back().method, "DELETE");
    EXPECT_NE(r.err.find("still running after 30 s; cancelled it"), std::string::npos) << r.err;
}

TEST(Cli, AFailedCoveragePartExitsOne) {
    FakeServer s;
    s.http().Post("/api/v1/calc/coverage", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 202,
                          {{"jobId", "j"},
                           {"state", "failed"},
                           {"parts", {{{"link", "control"}, {"state", "failed"}, {"error", "no DEM there"}}}}});
    });
    const Outcome r = run({"--server", s.url(), "calc", "coverage", "-"}, {}, "{}");
    EXPECT_EQ(r.status, Cli::kCheckFailed);
    EXPECT_NE(r.err.find("coverage of the control link failed: no DEM there"), std::string::npos) << r.err;
}

TEST(Cli, ARegionDownloadIsCheckedAgainstTheCatalog) {
    FakeServer s;
    const std::string zip = "PK\x03\x04 not really a zip";
    const std::string version(64, 'a');
    std::string catalogSha = ServiceToken::sha256Hex(zip);
    s.http().Get("/api/v1/regions/east", [&](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"id", "east"}, {"version", version}, {"sha256", catalogSha}});
    });
    s.http().Get("/api/v1/regions/east/packages/" + version + ".zip",
                 [&zip](const httplib::Request&, httplib::Response& res) { res.set_content(zip, "application/zip"); });
    const fs::path file = scratch("east.zip");
    const Outcome r = run({"--server", s.url(), "regions", "download", "east", "--out", file.string()});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    EXPECT_EQ(r.result()["bytes"], zip.size());
    std::ifstream in(file, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), zip);
    EXPECT_FALSE(fs::exists(file.string() + ".part"));

    catalogSha = std::string(64, '0');
    const Outcome bad = run({"--server", s.url(), "regions", "download", "east", "--out", file.string()});
    EXPECT_EQ(bad.status, Cli::kCheckFailed);
    EXPECT_NE(bad.err.find("the catalog says 0000"), std::string::npos) << bad.err;

    const Outcome missing = run({"--server", s.url(), "regions", "download", "west", "--out", (file.string() + "2")});
    EXPECT_EQ(missing.status, Cli::kServerError);
    EXPECT_FALSE(fs::exists(file.string() + "2.part"));
}

TEST(Cli, FlightLogsAreSentAsBytesWithTheirName) {
    FakeServer s;
    s.http().Post("/api/v1/flightlogs/decode", [](const httplib::Request& req, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"size", req.body.size()}, {"name", req.get_param_value("fileName")}});
    });
    const fs::path log = scratch("LOG00001.BFL");
    writeFile(log, std::string("H Product:Blackbox\n\0\1", 21));
    const Outcome r = run({"--server", s.url(), "flightlog", "decode", log.string()});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    EXPECT_EQ(r.result(), json({{"size", 21}, {"name", "LOG00001.BFL"}}));
    EXPECT_EQ(run({"--server", s.url(), "flightlog", "decode", "/no/such.bbl"}).status, Cli::kNoInput);
}

TEST(Cli, AMissingTokenGetsAHint) {
    FakeServer s;
    s.http().Post("/api/v1/calc/route", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 401, {{"message", "session required"}});
    });
    const Outcome r = run({"--server", s.url(), "calc", "route", "-"}, {}, "{}");
    EXPECT_EQ(r.status, Cli::kServerError);
    EXPECT_NE(r.err.find("need a service token"), std::string::npos) << r.err;
}

TEST(Cli, TokenNewNeedsNoServerAndGivesTheServerEntry) {
    const Outcome r = run({"token", "new", "--name", "nightly-ci"});
    EXPECT_EQ(r.status, Cli::kOk) << r.err;
    const json t = r.result();
    EXPECT_EQ(t["name"], "nightly-ci");
    EXPECT_TRUE(t["token"].get<std::string>().starts_with("rfmap_svc_"));
    EXPECT_EQ(t["sha256"], ServiceToken::sha256Hex(t["token"].get<std::string>()));
    EXPECT_NE(r.err.find(R"({"name":"nightly-ci","sha256":")"), std::string::npos) << r.err;
    EXPECT_EQ(run({"token", "new", "--name", "Bad Name"}).status, Cli::kUsage);
    EXPECT_EQ(run({"token", "new", "--name", "-x"}).status, Cli::kUsage);
    EXPECT_EQ(run({"token", "new"}).result()["name"], "ci");
}

TEST(Cli, ARedirectIsNamedNotFollowed) {
    FakeServer s;
    s.http().Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("https://maps.example.org/api/v1/health", 301);
    });
    const Outcome r = run({"--server", s.url(), "--token", "secret", "health"});
    EXPECT_EQ(r.status, Cli::kServerError);
    EXPECT_NE(r.err.find("redirects to https://maps.example.org/api/v1/health"), std::string::npos) << r.err;
    EXPECT_EQ(r.err.find("secret"), std::string::npos) << r.err;
}

TEST(Cli, FilesThatCannotBeWrittenExitSeventyThree) {
    FakeServer s;
    const std::string version(64, 'b');
    s.http().Get("/api/v1/regions/east", [&version](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 200, {{"id", "east"}, {"version", version}, {"sha256", std::string(64, '0')}});
    });
    s.http().Get("/api/v1/regions/east/packages/" + version + ".zip",
                 [](const httplib::Request&, httplib::Response& res) { res.set_content("zip", "application/zip"); });
    EXPECT_EQ(run({"--server", s.url(), "regions", "download", "east", "--out", "/no/such/dir/east.zip"}).status,
              Cli::kCantCreate);

    s.http().Post("/api/v1/calc/coverage", [](const httplib::Request&, httplib::Response& res) {
        FakeServer::reply(res, 202, {{"jobId", "j"}, {"state", "done"}, {"parts", json::array()}});
    });
    const fs::path file = scratch("not-a-dir");
    writeFile(file, "x");
    EXPECT_EQ(run({"--server", s.url(), "calc", "coverage", "-", "--out-dir", (file / "sub").string()}, {}, "{}").status,
              Cli::kCantCreate);
}
