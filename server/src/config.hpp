#pragma once

#include "common.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rfmap {

/// Server configuration: a JSON file, then environment overrides.
///
/// Overrides: RFMAP__<SECTION>__<KEY>=value sets config[section][key] (names matched without
/// regard to case; the value is parsed as JSON when it parses, else taken as a string). A few
/// common ones have short aliases: RFMAP_DATABASE_URL, RFMAP_ADMIN_TOKEN, RFMAP_PORT,
/// RFMAP_OPEN_METEO_URL, RFMAP_DEM, RFMAP_LANDCOVER.
class Config {
public:
    static Config load(const std::filesystem::path& file, const std::map<std::string, std::string>& env);
    static Config fromJson(json doc);
    static std::map<std::string, std::string> processEnvironment();

    struct Server {
        std::string listen = "0.0.0.0";
        int port = 8080;
        int threads = 16;
        bool trustProxy = false;  ///< use the last X-Forwarded-For hop as the client address
        std::size_t maxBodyBytes = 1u << 20;
        std::size_t maxUploadBytes = 64u << 20;
    } server;

    struct Database {
        std::string url = "postgresql://rfmap@localhost/rfmap";
        int poolSize = 8;
    } database;

    std::string adminToken;  ///< empty = admin API disabled

    struct Data {
        std::filesystem::path dem;
        std::filesystem::path landcover;
        /// The DEM's stated vertical error, as the provider publishes it. Reference data from the
        /// operator, not from us: without it the uncertainty band holds no terrain error, and
        /// results say so.
        std::optional<double> demStatedErrorM;
        std::string demStatedErrorSource;
        std::string demName;  ///< shown in results; defaults to manifest.json next to the DEM, then the file name
        std::string landcoverName;
    } data;

    std::filesystem::path schemasDir = "schemas";
    std::filesystem::path seedDir = "data/library";

    struct Modules {
        std::filesystem::path root = "/var/lib/rfmap/modules";
        std::filesystem::path worker = "/usr/lib/rfmap/rfmod-worker";
        std::filesystem::path trustStore = "/var/lib/rfmap/keys.json";
        bool isolateAll = false;
    } modules;

    struct WeatherTier {
        std::string name;    ///< "hourly" or "long-range"
        std::string models;  ///< Open-Meteo `models` parameter; empty = the instance's default
    };
    struct Weather {
        std::string baseUrl;  ///< self-hosted Open-Meteo, e.g. http://open-meteo:8080; empty = no forecasts
        std::vector<WeatherTier> tiers{{"hourly", ""}};
        int forecastDays = 16;
        int timeoutMs = 8000;
        int cacheMinutes = 30;
        std::string metaUrl;  ///< optional model meta.json giving the run time
        int staleAfterHours = 12;
    } weather;

    struct Cache {
        std::size_t demBlocksMb = 256;
        std::size_t resultMemoryMb = 64;
        std::size_t resultTableRows = 20000;
        std::size_t resultTableMb = 1024;
    } cache;

    struct Auth {
        int sessionTtlHours = 168;
        int pinMaxAttempts = 5;
        int pinLockoutMinutes = 15;
        std::string argon2Cost = "moderate";
    } auth;

    struct RateLimit {
        double perMinute;
        double burst;
    };
    /// Per client address and class: default, open (PIN attempts), create, calc, admin, upload.
    std::map<std::string, RateLimit> rateLimits{{"default", {600, 120}},  {"open", {10, 5}},
                                                {"create", {20, 10}},     {"calc", {120, 30}},
                                                {"admin", {60, 20}},      {"coverage", {6, 3}}};

    struct Coverage {
        int threads = 4;
        std::size_t maxCells = 250000;
        int maxJobs = 4;
        int keepMinutes = 120;
    } coverage;

    json raw;  ///< the merged document, for the admin page (secrets removed)
};

}  // namespace rfmap
