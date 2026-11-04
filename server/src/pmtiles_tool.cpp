#include "pmtiles_tool.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <thread>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr auto kPoll = std::chrono::milliseconds(100);
constexpr std::size_t kTailBytes = 600;

/// posix_spawn file actions: stdin from /dev/null, stdout and stderr to the log, nothing else open.
class SpawnFiles {
public:
    explicit SpawnFiles(const fs::path& logFile) : log_(logFile.string()) {
        posix_spawn_file_actions_init(&actions_);
        posix_spawn_file_actions_addopen(&actions_, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions_, 1, log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        posix_spawn_file_actions_adddup2(&actions_, 1, 2);
        // The server's sockets and database connections stay with the server.
        posix_spawn_file_actions_addclosefrom_np(&actions_, 3);
    }
    SpawnFiles(const SpawnFiles&) = delete;
    SpawnFiles& operator=(const SpawnFiles&) = delete;
    ~SpawnFiles() { posix_spawn_file_actions_destroy(&actions_); }

    [[nodiscard]] const posix_spawn_file_actions_t* get() const { return &actions_; }

private:
    std::string log_;
    posix_spawn_file_actions_t actions_{};
};

}  // namespace

std::vector<std::string> PmtilesTool::extractArguments(const Extract& job) {
    std::vector<std::string> args{"extract", job.input.string(), job.output.string(), "--quiet"};
    if (job.region.has_value()) args.push_back("--region=" + job.region->string());
    else args.push_back("--bbox=" + job.bbox.toArgument());
    if (job.maxZoom.has_value()) args.push_back(std::format("--maxzoom={}", *job.maxZoom));
    return args;
}

void PmtilesTool::extract(const Extract& job, const fs::path& logFile, const std::stop_token& stop) const {
    const int status = run(extractArguments(job), logFile, stop);
    if (status < 0) throw RegionBuildCancelled();
    if (status != 0) {
        throw RegionError(std::format("pmtiles extract of {} failed ({}): {}", job.input.filename().string(), status,
                                      tail(logFile)));
    }
}

std::string PmtilesTool::version(const fs::path& logFile) const {
    if (run({"version"}, logFile, std::stop_token()) != 0) throw RegionError("pmtiles version failed: " + tail(logFile));
    std::ifstream in(logFile);
    std::string line;
    std::getline(in, line);
    return line;
}

void PmtilesTool::checkExecutable() const {
    std::error_code ec;
    if (!executable_.is_absolute() || !fs::is_regular_file(executable_, ec) || access(executable_.c_str(), X_OK) != 0) {
        throw RegionError("regions.pmtiles must be the absolute path of the pmtiles executable: " + executable_.string());
    }
}

int PmtilesTool::run(const std::vector<std::string>& args, const fs::path& logFile, const std::stop_token& stop) const {
    checkExecutable();
    std::vector<std::string> owned{executable_.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::string path = "PATH=/usr/bin:/bin";
    std::array<char*, 2> envp{path.data(), nullptr};
    const SpawnFiles files(logFile);
    pid_t pid = 0;
    // The executable is operator configuration and checked above; the arguments are file names
    // the server chose and numbers it formatted, passed as separate words without a shell.
    if (const int error = posix_spawn(&pid, argv[0], files.get(), nullptr, argv.data(), envp.data()); error != 0) {  // NOSONAR(cpp:S2076)
        throw RegionError(std::format("cannot start {}: {}", executable_.string(), std::generic_category().message(error)));
    }
    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(pid, &status, WNOHANG)) == 0) {
        if (stop.stop_requested()) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(kPoll);
    }
    if (waited < 0) {  // the child cannot be waited for, so its result is unknown
        throw RegionError(std::format("cannot wait for {}: {}", executable_.string(), std::generic_category().message(errno)));
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

std::string PmtilesTool::tail(const fs::path& logFile) {
    std::ifstream in(logFile, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out = text.size() > kTailBytes ? text.substr(text.size() - kTailBytes) : text;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out.empty() ? "no output" : out;
}

}  // namespace rfmap
