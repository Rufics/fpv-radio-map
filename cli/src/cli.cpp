#include "cli.hpp"

#include "arguments.hpp"
#include "checks.hpp"
#include "service_token.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ostream>
#include <set>
#include <sstream>

namespace rfmap::cli {
namespace {

namespace fs = std::filesystem;

/// An input file the command names cannot be read: exit status 66.
class InputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// An output file cannot be written: exit status 73.
class OutputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr std::string_view kUsageText = R"(usage: rfmap [--server URL] [--token TOKEN | --token-file FILE] COMMAND [options]

Calculations and data from an FPV Radio Map server, as JSON on stdout.

Server and credentials (or the environment: RFMAP_SERVER, RFMAP_TOKEN, RFMAP_TOKEN_FILE):
  --server URL          https://maps.example.org; the API is at <URL>/api/v1
  --token TOKEN         a service token; prefer RFMAP_TOKEN, which process lists do not show
  --token-file FILE     read the token from FILE
  --timeout SECONDS     per request (default 300)
  --retries N           after 429/503 and, for reads, failed connections (default 3)
  --output FILE         write the JSON result to FILE instead of stdout
  --compact             one line of JSON instead of indented

Commands:
  health                          whether the server is up; exit 1 unless it says ok
  version                         this client's version and the server's
  calc path --from LAT,LON,AGL --to LAT,LON,AGL LINK [--time UTC] [--modules FILE]
            LINK is one of:
              --link FILE [--kind control|video]
              --links FILE [--evaluate both|control|video]
              --radio ID --tx-power DBM --ground-antenna ID --air-antenna ID
                --required-margin DB [--kind control|video]
  calc route MISSION.json         every leg of a mission (schemas/mission.schema.json)
  calc coverage MISSION.json [--out-dir DIR] [--no-wait] [--poll SECONDS] [--wait SECONDS]
  elevation --from LAT,LON --to LAT,LON
  library list KIND | library get KIND ID | library history KIND ID
  weather point --lat LAT --lon LON [--time UTC] | weather horizon
  modules | map
  regions list | regions get ID | regions download ID --out FILE.zip
  flightlog decode FILE [--name NAME]
  api GET|POST|PUT|DELETE PATH [--data FILE]   any other call; PATH is below /api/v1
  token new [--name NAME]         a service token and its entry for the server's services.tokens

A FILE of "-" is stdin. Calculations need a service token; the rest of the data API does not.

Checks for CI (calc path and calc route); a result that falls short exits with 1:
  --fail-on warn|blocked          a leg status at least this bad (calc path: an unreachable link)
  --min-margin DB                 an evaluated link whose worst margin is below DB
  --summary                       one line per leg or link on stderr

Exit status: 0 ok, 1 a check failed, 2 the server answered an error, 3 the server could not be
reached, 64 usage, 66 an input file cannot be read, 73 an output file cannot be written.
)";

/// Percent-encodes a query value: everything but the unreserved characters of RFC 3986.
std::string encodeQuery(std::string_view value) {
    std::string out;
    for (const char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            out += ch;
        } else {
            out += std::format("%{:02X}", static_cast<unsigned char>(ch));
        }
    }
    return out;
}

/// `text` as one URL path segment of the server's own pattern; UsageError otherwise, so no
/// argument can climb out of the path it is put into.
std::string segment(const std::string& text, std::string_view extra, const char* what) {
    const auto allowed = [extra](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::islower(c) != 0 || std::isdigit(c) != 0 || extra.find(ch) != std::string_view::npos;
    };
    if (text.empty() || !std::ranges::all_of(text, allowed)) {
        throw UsageError(std::format("{} \"{}\" is not one the server knows", what, text));
    }
    return text;
}

json point(const std::vector<double>& c) { return {{"lat", c[0]}, {"lon", c[1]}, {"aglM", c[2]}}; }

std::string latLon(const std::vector<double>& c) { return std::format("{},{}", c[0], c[1]); }

const std::string& kindOf(const Arguments& args, const std::string& fallback) {
    static const std::set<std::string, std::less<>> kinds{"control", "video"};
    const auto it = kinds.find(args.get("--kind").value_or(fallback));
    if (it == kinds.end()) throw UsageError("--kind takes control or video");
    return *it;
}

Requirements requirementsOf(const Arguments& args) {
    Requirements r;
    if (const auto failOn = args.get("--fail-on")) r.failOnSeverity = Requirements::severityOf(*failOn);
    r.minMarginDb = args.number("--min-margin");
    return r;
}

/// A coverage job's parts as "control 40%, video 10%".
std::string progressLine(const json& job) {
    std::string out;
    for (const json& part : job.value("parts", json::array())) {
        if (!out.empty()) out += ", ";
        out += std::format("{} {:.0f}%", part.value("link", "?"), 100.0 * part.value("progress", 0.0));
    }
    return out;
}

bool jobRunning(const json& job) {
    const std::string state = job.value("state", "");
    return state == "queued" || state == "running";
}

}  // namespace

Cli::Cli(std::string version, Io io) : version_(std::move(version)), io_(std::move(io)) {}

void Cli::usage(std::ostream& out) { out << kUsageText; }

int Cli::run(std::span<const std::string> words) {
    try {
        const Arguments args(words);
        if (args.has("--version")) {
            io_.out << "rfmap " << version_ << "\n";
            return kOk;
        }
        if (const bool help = args.has("--help") || (!args.positional().empty() && args.positional()[0] == "help");
            help || args.positional().empty()) {
            usage(help ? io_.out : io_.err);
            return help ? kOk : kUsage;
        }
        return dispatch(args);
    } catch (const UsageError& e) {
        io_.err << "rfmap: " << e.what() << "\n(rfmap --help lists the commands and their options)\n";
        return kUsage;
    } catch (const InputError& e) {
        io_.err << "rfmap: " << e.what() << "\n";
        return kNoInput;
    } catch (const OutputError& e) {
        io_.err << "rfmap: " << e.what() << "\n";
        return kCantCreate;
    } catch (const LocalFileError& e) {
        io_.err << "rfmap: " << e.what() << "\n";
        return kCantCreate;
    } catch (const ApiError& e) {
        io_.err << "rfmap: " << e.what() << "\n";
        if (e.body().is_object() && e.body().contains("issues") && e.body().at("issues").is_array()) {
            for (const json& issue : e.body().at("issues")) {
                io_.err << "  " << issue.value("path", "") << ": " << issue.value("message", issue.dump()) << "\n";
            }
        }
        if (e.status() == 401) {
            io_.err << "(calculations need a service token the server knows, in RFMAP_TOKEN or --token-file; "
                       "rfmap token new makes one)\n";
        }
        return kServerError;
    } catch (const ConnectionError& e) {
        io_.err << "rfmap: cannot reach the server: " << e.what() << "\n";
        return kUnreachable;
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): the process boundary, any other failure is exit 70
        io_.err << "rfmap: " << e.what() << "\n";
        return kInternal;
    }
}

int Cli::dispatch(const Arguments& args) const {
    const std::string& command = args.positional()[0];
    if (command == "health") return health(args);
    if (command == "version") return version(args);
    if (command == "calc") return calc(args);
    if (command == "elevation") return elevation(args);
    if (command == "library") return library(args);
    if (command == "weather") return weather(args);
    if (command == "modules" || command == "map") {
        args.requireOnly({}, 1);
        return simpleGet(args, command == "modules" ? "/modules" : "/config/map");
    }
    if (command == "regions") return regions(args);
    if (command == "flightlog") return flightlog(args);
    if (command == "api") return api(args);
    if (command == "token") return token(args);
    throw UsageError("unknown command " + command);
}

ApiClient Cli::client(const Arguments& args) const {
    const auto envValue = [this](const char* name) -> std::optional<std::string> {
        const auto it = io_.env.find(name);
        if (it == io_.env.end() || it->second.empty()) return std::nullopt;
        return it->second;
    };
    ClientOptions o;
    o.server = args.get("--server").value_or(envValue("RFMAP_SERVER").value_or(""));
    if (o.server.empty()) throw UsageError("no server: pass --server URL or set RFMAP_SERVER");
    auto tokenFile = args.get("--token-file");
    if (!tokenFile) tokenFile = envValue("RFMAP_TOKEN_FILE");
    if (const auto t = args.get("--token")) {
        o.token = *t;
    } else if (tokenFile) {
        const std::string& file = *tokenFile;
        o.token = readBytes(file);
        while (!o.token.empty() && std::isspace(static_cast<unsigned char>(o.token.back())) != 0) o.token.pop_back();
    } else {
        o.token = envValue("RFMAP_TOKEN").value_or("");
    }
    if (const auto t = args.number("--timeout"); t.has_value()) {
        if (*t <= 0) throw UsageError("--timeout must be positive");
        o.timeout = std::chrono::seconds(static_cast<long long>(std::ceil(*t)));
    }
    if (const auto r = args.number("--retries"); r.has_value()) {
        if (*r < 0 || *r > 10 || *r != std::floor(*r)) throw UsageError("--retries takes a whole number from 0 to 10");
        o.retries = static_cast<int>(*r);
    }
    o.userAgent = "rfmap-cli/" + version_;
    return ApiClient(std::move(o), io_.sleep);
}

void Cli::emit(const Arguments& args, const json& result) const {
    const int indent = args.has("--compact") ? -1 : 2;
    const std::string text = result.dump(indent, ' ', false, json::error_handler_t::replace) + "\n";
    if (const auto file = args.get("--output")) {
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) throw OutputError("cannot write " + *file);
        return;
    }
    io_.out << text;
    io_.out.flush();
}

std::string Cli::readBytes(const std::string& file) const {
    if (file == "-") return {std::istreambuf_iterator<char>(io_.in), std::istreambuf_iterator<char>()};
    std::ifstream in(file, std::ios::binary);
    if (!in) throw InputError("cannot read " + file);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

json Cli::readJson(const std::string& file) const {
    const std::string text = readBytes(file);
    try {
        return json::parse(text);
    } catch (const json::exception& e) {
        throw InputError(std::format("{} is not JSON: {}", file == "-" ? "stdin" : file, e.what()));
    }
}

int Cli::verdict(const std::vector<std::string>& shortfalls) const {
    for (const std::string& s : shortfalls) io_.err << "check failed: " << s << "\n";
    return shortfalls.empty() ? kOk : kCheckFailed;
}

int Cli::health(const Arguments& args) const {
    args.requireOnly({}, 1);
    ApiClient api = client(args);
    const json h = api.get("/health");
    emit(args, h);
    return h.is_object() && h.value("ok", false) ? kOk : kCheckFailed;
}

int Cli::version(const Arguments& args) const {
    args.requireOnly({}, 1);
    json out = {{"client", version_}};
    if (const auto env = io_.env.find("RFMAP_SERVER");
        args.has("--server") || (env != io_.env.end() && !env->second.empty())) {
        ApiClient api = client(args);
        out["server"] = api.get("/health").value("version", "");
    }
    emit(args, out);
    return kOk;
}

int Cli::calc(const Arguments& args) const {
    const std::string& what = args.word(1, "what to calculate: path, route or coverage");
    if (what == "path") return calcPath(args);
    if (what == "route") return calcRoute(args);
    if (what == "coverage") return calcCoverage(args);
    throw UsageError("calc takes path, route or coverage, not " + what);
}

int Cli::calcPath(const Arguments& args) const {
    args.requireOnly({"--from", "--to", "--link", "--links", "--kind", "--evaluate", "--radio", "--tx-power",
                      "--ground-antenna", "--air-antenna", "--required-margin", "--time", "--modules", "--fail-on",
                      "--min-margin", "--summary"},
                     2);
    const Requirements required = requirementsOf(args);
    json body = {{"from", point(Arguments::coordinates(args.need("--from"), "--from", true))},
                 {"to", point(Arguments::coordinates(args.need("--to"), "--to", true))}};
    if (args.has("--link") && args.has("--links")) throw UsageError("give --link or --links, not both");
    if (const auto links = args.get("--links")) {
        body["links"] = readJson(*links);
        if (const auto evaluate = args.get("--evaluate")) body["evaluate"] = *evaluate;
    } else if (const auto link = args.get("--link")) {
        body["kind"] = kindOf(args, "control");
        body["link"] = readJson(*link);
    } else {
        body["kind"] = kindOf(args, "control");
        body["link"] = {{"radio", args.need("--radio")},
                        {"txPowerDbm", Arguments::parseNumber(args.need("--tx-power"), "--tx-power")},
                        {"groundAntenna", args.need("--ground-antenna")},
                        {"airAntenna", args.need("--air-antenna")},
                        {"requiredMarginDb", Arguments::parseNumber(args.need("--required-margin"), "--required-margin")}};
    }
    if (const auto time = args.get("--time")) body["timeUtc"] = *time;
    if (const auto modules = args.get("--modules")) body["modules"] = readJson(*modules);

    ApiClient api = client(args);
    const json result = api.post("/calc/path", body);
    emit(args, result);
    if (args.has("--summary")) io_.err << pathSummary(result);
    return verdict(pathShortfalls(result, required));
}

int Cli::calcRoute(const Arguments& args) const {
    args.requireOnly({"--fail-on", "--min-margin", "--summary"}, 3);
    const Requirements required = requirementsOf(args);
    const json mission = readJson(args.word(2, "the mission file (MISSION.json, or - for stdin)"));
    ApiClient api = client(args);
    const json result = api.post("/calc/route", mission);
    emit(args, result);
    if (args.has("--summary")) io_.err << routeSummary(result);
    return verdict(routeShortfalls(result, required));
}

int Cli::calcCoverage(const Arguments& args) const {
    args.requireOnly({"--out-dir", "--no-wait", "--poll", "--wait"}, 3);
    const json mission = readJson(args.word(2, "the mission file (MISSION.json, or - for stdin)"));
    const double poll = args.number("--poll").value_or(2.0);
    const double patience = args.number("--wait").value_or(1800.0);
    if (poll <= 0 || patience < 0) throw UsageError("--poll must be positive and --wait not negative");

    ApiClient api = client(args);
    json job = api.postWithStatus("/calc/coverage", mission).second;
    const std::string id = segment(job.value("jobId", ""), "_-ABCDEFGHIJKLMNOPQRSTUVWXYZ", "job id");
    if (args.has("--no-wait")) {
        emit(args, job);
        return kOk;
    }
    if (!awaitJob(api, id, job, poll, patience)) return kCheckFailed;

    json out = {{"job", job}};
    const bool complete = fetchGrids(api, id, job, args.get("--out-dir"), out);
    emit(args, out);
    return complete && job.value("state", "") == "done" ? kOk : kCheckFailed;
}

bool Cli::awaitJob(ApiClient& api, const std::string& id, json& job, double poll, double patience) const {
    double waited = 0.0;
    std::string lastProgress;
    while (jobRunning(job)) {
        if (waited >= patience) {
            (void)api.remove("/jobs/" + id);
            io_.err << std::format("rfmap: coverage job {} still running after {:.0f} s; cancelled it\n", id, waited);
            return false;
        }
        io_.sleep(std::chrono::milliseconds(static_cast<long long>(poll * 1000.0)));
        waited += poll;
        job = api.get("/jobs/" + id);
        if (const std::string line = progressLine(job); line != lastProgress) {
            io_.err << "coverage " << id << ": " << line << "\n";
            lastProgress = line;
        }
    }
    return true;
}

bool Cli::fetchGrids(ApiClient& api, const std::string& id, const json& job, const std::optional<std::string>& outDir,
                     json& out) const {
    if (outDir) {
        std::error_code made;
        fs::create_directories(*outDir, made);
        if (made) throw OutputError("cannot create " + *outDir + ": " + made.message());
    }
    bool complete = true;
    for (const json& part : job.value("parts", json::array())) {
        const std::string state = part.value("state", "");
        const std::string link = part.value("link", "");
        if (state != "done" && state != "cached") {
            complete = false;
            const std::string why = part.contains("error") ? ": " + part.value("error", "") : "";
            io_.err << std::format("rfmap: coverage of the {} link {}{}\n", link, state, why);
            continue;
        }
        const json grid = api.get("/jobs/" + id + "/coverage/" + segment(link, "", "link") + ".json");
        if (!outDir) {
            out["grids"][link] = grid;
            continue;
        }
        const fs::path file = fs::path(*outDir) / (link + ".json");
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << grid.dump();
        f.close();
        if (!f) throw OutputError("cannot write " + file.string());
        out["files"][link] = file.string();
    }
    return complete;
}

int Cli::elevation(const Arguments& args) const {
    args.requireOnly({"--from", "--to"}, 1);
    const auto from = Arguments::coordinates(args.need("--from"), "--from", false);
    const auto to = Arguments::coordinates(args.need("--to"), "--to", false);
    return simpleGet(args, "/elevation/profile?from=" + encodeQuery(latLon(from)) + "&to=" + encodeQuery(latLon(to)));
}

int Cli::library(const Arguments& args) const {
    const std::string& what = args.word(1, "list, get or history");
    const std::string kind = segment(args.word(2, "the library kind, e.g. radio-profiles"), "-", "library kind");
    if (what == "list") {
        args.requireOnly({}, 3);
        return simpleGet(args, "/library/" + kind);
    }
    if (what != "get" && what != "history") throw UsageError("library takes list, get or history, not " + what);
    args.requireOnly({}, 4);
    const std::string id = segment(args.word(3, "the item id"), "._-", "library item id");
    return simpleGet(args, "/library/" + kind + "/" + id + (what == "history" ? "/history" : ""));
}

int Cli::weather(const Arguments& args) const {
    const std::string& what = args.word(1, "point or horizon");
    if (what == "horizon") {
        args.requireOnly({}, 2);
        return simpleGet(args, "/weather/horizon");
    }
    if (what != "point") throw UsageError("weather takes point or horizon, not " + what);
    args.requireOnly({"--lat", "--lon", "--time"}, 2);
    const double lat = Arguments::parseNumber(args.need("--lat"), "--lat");
    const double lon = Arguments::parseNumber(args.need("--lon"), "--lon");
    std::string path = std::format("/weather/point?lat={}&lon={}", lat, lon);
    if (const auto time = args.get("--time")) path += "&timeUtc=" + encodeQuery(*time);
    return simpleGet(args, path);
}

int Cli::simpleGet(const Arguments& args, const std::string& path) const {
    ApiClient api = client(args);
    emit(args, api.get(path));
    return kOk;
}

int Cli::regions(const Arguments& args) const {
    const std::string& what = args.word(1, "list, get or download");
    if (what == "list") {
        args.requireOnly({}, 2);
        return simpleGet(args, "/regions");
    }
    const std::string id = segment(args.word(2, "the region id"), "-", "region id");
    if (what == "get") {
        args.requireOnly({}, 3);
        return simpleGet(args, "/regions/" + id);
    }
    if (what != "download") throw UsageError("regions takes list, get or download, not " + what);
    args.requireOnly({"--out"}, 3);
    const std::string file = args.need("--out");
    ApiClient api = client(args);
    const json region = api.get("/regions/" + id);
    const std::string version = segment(region.value("version", ""), "", "package version");
    const ApiClient::Download d = api.download("/regions/" + id + "/packages/" + version + ".zip", file);
    const std::string expected = region.value("sha256", "");
    emit(args, {{"region", id}, {"version", version}, {"file", file}, {"bytes", d.bytes}, {"sha256", d.sha256}});
    if (d.sha256 != expected) {
        io_.err << std::format("rfmap: {} has SHA-256 {}, the catalog says {}\n", file, d.sha256, expected);
        return kCheckFailed;
    }
    return kOk;
}

int Cli::flightlog(const Arguments& args) const {
    if (const std::string& what = args.word(1, "decode"); what != "decode") {
        throw UsageError("flightlog takes decode, not " + what);
    }
    args.requireOnly({"--name"}, 3);
    const std::string& file = args.word(2, "the flight log file");
    const std::string bytes = readBytes(file);
    const std::string name = args.get("--name").value_or(file == "-" ? "stdin" : fs::path(file).filename().string());
    ApiClient api = client(args);
    emit(args, api.postBytes("/flightlogs/decode?fileName=" + encodeQuery(name), bytes, "application/octet-stream"));
    return kOk;
}

int Cli::api(const Arguments& args) const {
    args.requireOnly({"--data"}, 3);
    std::string method = args.word(1, "the method: GET, POST, PUT or DELETE");
    std::ranges::transform(method, method.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const std::string& path = args.word(2, "the path below /api/v1, e.g. /library/antennas");
    if (!path.starts_with('/')) throw UsageError("the path must start with /, e.g. /library/antennas");
    const auto data = args.get("--data");
    ApiClient client = this->client(args);
    json result;
    if (method == "GET") {
        result = client.get(path);
    } else if (method == "DELETE") {
        result = client.remove(path);
    } else if (method == "POST" || method == "PUT") {
        const json body = data ? readJson(*data) : json::object();
        result = method == "POST" ? client.post(path, body) : client.put(path, body);
    } else {
        throw UsageError("api takes GET, POST, PUT or DELETE, not " + method);
    }
    emit(args, result);
    return kOk;
}

int Cli::token(const Arguments& args) const {
    if (const std::string& what = args.word(1, "new"); what != "new") throw UsageError("token takes new, not " + what);
    args.requireOnly({"--name"}, 2);
    const std::string name = segment(args.get("--name").value_or("ci"), "._-", "token name");
    if (name.size() > 64 || std::ispunct(static_cast<unsigned char>(name.front())) != 0) {
        throw UsageError("a token name starts with a letter or digit and has at most 64 characters");
    }
    const ServiceToken t = ServiceToken::generate();
    emit(args, {{"name", name}, {"token", t.token}, {"sha256", t.sha256}});
    io_.err << "Give the token to the service (RFMAP_TOKEN) and keep it secret. Add this entry to the server's\n"
               "services.tokens; the server keeps only the SHA-256:\n  "
            << json{{"name", name}, {"sha256", t.sha256}}.dump() << "\n";
    return kOk;
}

}  // namespace rfmap::cli
