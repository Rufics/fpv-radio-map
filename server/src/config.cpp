#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

extern char** environ;

namespace rfmap {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

json parseValue(const std::string& v) {
    try {
        return json::parse(v);
    } catch (const json::exception&) {
        return v;
    }
}

/// Finds a key in `obj` ignoring case, so RFMAP__CACHE__DEMBLOCKSMB reaches "demBlocksMb".
std::string matchKey(const json& obj, const std::string& wanted) {
    if (obj.is_object()) {
        for (const auto& [k, v] : obj.items()) {
            if (lower(k) == lower(wanted)) return k;
        }
    }
    // Keys absent from the file still get their proper spelling.
    static const char* known[] = {
        "server", "listen", "port", "threads", "trustProxy", "maxBodyBytes", "maxUploadBytes", "database", "url",
        "poolSize", "admin", "token", "data", "dem", "landcover", "demStatedErrorM", "demStatedErrorSource", "demName",
        "landcoverName", "schemasDir", "seedDir", "modules", "root", "worker", "trustStore", "isolateAll", "weather",
        "baseUrl", "tiers", "forecastDays", "timeoutMs", "cacheMinutes", "metaUrl", "staleAfterHours", "cache",
        "demBlocksMb", "resultMemoryMb", "resultTableRows", "resultTableMb", "auth", "sessionTtlHours",
        "pinMaxAttempts", "pinLockoutMinutes", "argon2Cost", "rateLimits", "perMinute", "burst", "coverage",
        "maxCells", "maxJobs", "keepMinutes", "map"};
    for (const char* k : known) {
        if (lower(k) == lower(wanted)) return k;
    }
    return wanted;
}

void setPath(json& doc, const std::vector<std::string>& path, const json& value) {
    json* node = &doc;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (!node->is_object()) *node = json::object();
        const std::string key = matchKey(*node, path[i]);
        if (i + 1 == path.size()) {
            (*node)[key] = value;
        } else {
            node = &(*node)[key];
        }
    }
}

template <class T>
void read(const json& obj, const char* key, T& out) {
    if (obj.is_object() && obj.contains(key) && !obj.at(key).is_null()) out = obj.at(key).get<T>();
}

void readPath(const json& obj, const char* key, std::filesystem::path& out) {
    if (obj.is_object() && obj.contains(key) && obj.at(key).is_string()) out = obj.at(key).get<std::string>();
}

}  // namespace

std::map<std::string, std::string> Config::processEnvironment() {
    std::map<std::string, std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const auto eq = kv.find('=');
        if (eq != std::string::npos && kv.rfind("RFMAP", 0) == 0) env[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return env;
}

Config Config::load(const std::filesystem::path& file, const std::map<std::string, std::string>& env) {
    json doc = json::object();
    if (!file.empty()) {
        std::ifstream in(file);
        if (!in) throw std::runtime_error("cannot read config file " + file.string());
        doc = json::parse(in, nullptr, true, true);
    }
    static const std::map<std::string, std::vector<std::string>> aliases = {
        {"RFMAP_DATABASE_URL", {"database", "url"}},  {"RFMAP_ADMIN_TOKEN", {"admin", "token"}},
        {"RFMAP_PORT", {"server", "port"}},          {"RFMAP_OPEN_METEO_URL", {"weather", "baseUrl"}},
        {"RFMAP_DEM", {"data", "dem"}},              {"RFMAP_LANDCOVER", {"data", "landcover"}}};
    for (const auto& [name, value] : env) {
        if (const auto a = aliases.find(name); a != aliases.end()) {
            // Port and similar numbers parse as JSON; tokens and URLs stay strings.
            setPath(doc, a->second, name == "RFMAP_PORT" ? parseValue(value) : json(value));
            continue;
        }
        if (name.rfind("RFMAP__", 0) != 0) continue;
        std::vector<std::string> path;
        std::string rest = name.substr(7);
        std::size_t pos = 0;
        while ((pos = rest.find("__")) != std::string::npos) {
            path.push_back(rest.substr(0, pos));
            rest = rest.substr(pos + 2);
        }
        path.push_back(rest);
        setPath(doc, path, parseValue(value));
    }
    return fromJson(std::move(doc));
}

Config Config::fromJson(json doc) {
    Config c;
    const json& s = doc.value("server", json::object());
    read(s, "listen", c.server.listen);
    read(s, "port", c.server.port);
    read(s, "threads", c.server.threads);
    read(s, "trustProxy", c.server.trustProxy);
    read(s, "maxBodyBytes", c.server.maxBodyBytes);
    read(s, "maxUploadBytes", c.server.maxUploadBytes);

    const json& db = doc.value("database", json::object());
    read(db, "url", c.database.url);
    read(db, "poolSize", c.database.poolSize);

    read(doc.value("admin", json::object()), "token", c.adminToken);

    const json& d = doc.value("data", json::object());
    readPath(d, "dem", c.data.dem);
    readPath(d, "landcover", c.data.landcover);
    if (d.contains("demStatedErrorM") && d.at("demStatedErrorM").is_number()) {
        c.data.demStatedErrorM = d.at("demStatedErrorM").get<double>();
    }
    read(d, "demStatedErrorSource", c.data.demStatedErrorSource);
    read(d, "demName", c.data.demName);
    read(d, "landcoverName", c.data.landcoverName);

    readPath(doc, "schemasDir", c.schemasDir);
    readPath(doc, "seedDir", c.seedDir);

    const json& m = doc.value("modules", json::object());
    readPath(m, "root", c.modules.root);
    readPath(m, "worker", c.modules.worker);
    readPath(m, "trustStore", c.modules.trustStore);
    read(m, "isolateAll", c.modules.isolateAll);

    const json& w = doc.value("weather", json::object());
    read(w, "baseUrl", c.weather.baseUrl);
    read(w, "forecastDays", c.weather.forecastDays);
    read(w, "timeoutMs", c.weather.timeoutMs);
    read(w, "cacheMinutes", c.weather.cacheMinutes);
    read(w, "metaUrl", c.weather.metaUrl);
    read(w, "staleAfterHours", c.weather.staleAfterHours);
    if (w.contains("tiers") && w.at("tiers").is_array()) {
        c.weather.tiers.clear();
        for (const json& t : w.at("tiers")) {
            c.weather.tiers.push_back({t.value("name", "hourly"), t.value("models", "")});
        }
    }

    const json& ca = doc.value("cache", json::object());
    read(ca, "demBlocksMb", c.cache.demBlocksMb);
    read(ca, "resultMemoryMb", c.cache.resultMemoryMb);
    read(ca, "resultTableRows", c.cache.resultTableRows);
    read(ca, "resultTableMb", c.cache.resultTableMb);

    const json& a = doc.value("auth", json::object());
    read(a, "sessionTtlHours", c.auth.sessionTtlHours);
    read(a, "pinMaxAttempts", c.auth.pinMaxAttempts);
    read(a, "pinLockoutMinutes", c.auth.pinLockoutMinutes);
    read(a, "argon2Cost", c.auth.argon2Cost);

    if (doc.contains("rateLimits") && doc.at("rateLimits").is_object()) {
        for (const auto& [k, v] : doc.at("rateLimits").items()) {
            c.rateLimits[k] = {v.value("perMinute", 60.0), v.value("burst", 10.0)};
        }
    }

    const json& cv = doc.value("coverage", json::object());
    read(cv, "threads", c.coverage.threads);
    read(cv, "maxCells", c.coverage.maxCells);
    read(cv, "maxJobs", c.coverage.maxJobs);
    read(cv, "keepMinutes", c.coverage.keepMinutes);

    if (c.server.port <= 0 || c.server.port > 65535) throw std::invalid_argument("server.port out of range");
    if (c.auth.pinMaxAttempts < 1) throw std::invalid_argument("auth.pinMaxAttempts must be >= 1");
    if (c.data.demStatedErrorM && !(*c.data.demStatedErrorM >= 0.0)) {
        throw std::invalid_argument("data.demStatedErrorM must be >= 0");
    }

    c.raw = doc;
    if (c.raw.contains("admin")) c.raw["admin"]["token"] = "(set)";
    if (c.raw.contains("database") && c.raw["database"].contains("url")) c.raw["database"]["url"] = "(set)";
    return c;
}

}  // namespace rfmap
