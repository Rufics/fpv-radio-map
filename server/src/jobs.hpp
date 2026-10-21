#pragma once

#include "calculator.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace rfmap {

/// Coverage jobs. Each job has one part per evaluated link; a finished part's grid is
/// available at once while the other part still runs. Jobs belong to the mission of the
/// session that started them and are forgotten after `keepMinutes`.
class JobManager {
public:
    JobManager(Calculator& calc, int jobThreads, int cellThreads, int keepMinutes, Logger& log);
    ~JobManager();

    json start(std::string_view missionId, const json& mission);
    [[nodiscard]] json status(const std::string& missionId, const std::string& jobId);
    void cancel(const std::string& missionId, const std::string& jobId);
    /// The part's grid; HttpError 409 while it is still running.
    [[nodiscard]] json grid(const std::string& missionId, const std::string& jobId, const std::string& link);
    [[nodiscard]] std::size_t running() const;
    [[nodiscard]] JobStats stats();

private:
    struct Part {
        std::string name;  ///< for display
        std::string link;  ///< control | video
        std::string state = "waiting";
        double progress = 0.0;
        std::string error;
        json grid;
    };
    struct Job {
        std::string id;
        std::string missionId;
        std::string state = "queued";
        std::vector<Part> parts;
        std::atomic<bool> cancelled{false};
        TimePoint created;
    };
    std::shared_ptr<Job> find(std::string_view missionId, std::string_view jobId);
    json toJson(const Job& j) const;
    /// Runs the job's parts in order on a runner thread; `failed` is set by a part that failed.
    void runJob(Job& job, const json& mission);
    void runPart(const Job& job, Part& part, const json& mission, bool& failed);
    /// Marks a part whose calculation threw; `failed` is set unless the job was cancelled.
    void recordFailure(const Job& job, Part& part, const char* what, bool& failed);
    void sweep();

    Calculator& calc_;
    int keepMinutes_;
    Logger& log_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Job>, std::less<>> jobs_;
    std::uint64_t started_ = 0;  ///< guarded by mutex_
    ThreadPool cells_;
    ThreadPool runners_;  ///< declared last: destroyed first, so running jobs finish before the cell pool goes
};

}  // namespace rfmap
