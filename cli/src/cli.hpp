#pragma once

#include "api_client.hpp"

#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace rfmap::cli {

class Arguments;
struct Requirements;

/// What the process gives the tool: its streams, the RFMAP_* environment, and how to wait.
struct Io {
    std::istream& in;
    std::ostream& out;  ///< results: JSON only, so scripts can pipe them
    std::ostream& err;  ///< errors, summaries and hints for people
    std::map<std::string, std::string, std::less<>> env;
    ApiClient::Sleep sleep;
};

/// rfmap, the FPV Radio Map command-line client: calculations and data from a server's HTTP API
/// for scripts, CI and other services. One method per command; results go to stdout as JSON.
class Cli {
public:
    /// Exit statuses, so CI can tell a failed check from a server that is down.
    static constexpr int kOk = 0;
    static constexpr int kCheckFailed = 1;  ///< --fail-on / --min-margin, health not ok, a job failed
    static constexpr int kServerError = 2;  ///< the server answered an error (4xx/5xx after retries)
    static constexpr int kUnreachable = 3;  ///< no answer: connection refused, DNS, timeout, TLS
    static constexpr int kUsage = 64;       ///< EX_USAGE: the command line is wrong
    static constexpr int kNoInput = 66;     ///< EX_NOINPUT: an input file cannot be read
    static constexpr int kCantCreate = 73;  ///< EX_CANTCREAT: an output file cannot be written
    static constexpr int kInternal = 70;    ///< EX_SOFTWARE: anything else

    Cli(std::string version, Io io);

    /// Runs the command line (without the program name); returns the exit status.
    [[nodiscard]] int run(std::span<const std::string> words);
    static void usage(std::ostream& out);

private:
    [[nodiscard]] int dispatch(const Arguments& args) const;
    [[nodiscard]] ApiClient client(const Arguments& args) const;
    /// Writes `result` to --output or stdout.
    void emit(const Arguments& args, const json& result) const;
    /// Reads a JSON document from a file, or from stdin for "-".
    [[nodiscard]] json readJson(const std::string& file) const;
    [[nodiscard]] std::string readBytes(const std::string& file) const;
    /// Prints each shortfall and returns kCheckFailed, or kOk when there is none.
    [[nodiscard]] int verdict(const std::vector<std::string>& shortfalls) const;

    [[nodiscard]] int health(const Arguments& args) const;
    [[nodiscard]] int version(const Arguments& args) const;
    [[nodiscard]] int calc(const Arguments& args) const;
    [[nodiscard]] int calcPath(const Arguments& args) const;
    [[nodiscard]] int calcRoute(const Arguments& args) const;
    [[nodiscard]] int calcCoverage(const Arguments& args) const;
    /// Polls the coverage job until it ends; false when it outlasted `patience` and was cancelled.
    [[nodiscard]] bool awaitJob(ApiClient& api, const std::string& id, json& job, double poll, double patience) const;
    /// Puts each finished part's grid into out["grids"], or into a file under `outDir` listed in
    /// out["files"]; false when a part did not finish.
    [[nodiscard]] bool fetchGrids(ApiClient& api, const std::string& id, const json& job,
                                  const std::optional<std::string>& outDir, json& out) const;
    [[nodiscard]] int elevation(const Arguments& args) const;
    [[nodiscard]] int library(const Arguments& args) const;
    [[nodiscard]] int weather(const Arguments& args) const;
    [[nodiscard]] int simpleGet(const Arguments& args, const std::string& path) const;
    [[nodiscard]] int regions(const Arguments& args) const;
    [[nodiscard]] int flightlog(const Arguments& args) const;
    [[nodiscard]] int api(const Arguments& args) const;
    [[nodiscard]] int token(const Arguments& args) const;

    std::string version_;
    Io io_;
};

}  // namespace rfmap::cli
