#include "jobs.hpp"

#include "security.hpp"

#include <exception>
#include <stdexcept>

namespace rfmap {

namespace {

/// One part's progress, written under the manager's lock; cancelling is the job's flag.
class PartProgress final : public CoverageProgress {
public:
    PartProgress(std::mutex& mutex, double& progress, const std::atomic<bool>& cancelled)
        : mutex_(mutex), progress_(progress), cancelled_(cancelled) {}
    void report(double fraction) override {
        std::lock_guard lock(mutex_);
        progress_ = fraction;
    }
    [[nodiscard]] bool cancelled() const override { return cancelled_.load(); }

private:
    std::mutex& mutex_;
    double& progress_;
    const std::atomic<bool>& cancelled_;
};

/// Sets a job's final state when its run ends, also when a part threw something the part's
/// handlers do not contain, so a job never stays "running".
class JobFinisher {
public:
    JobFinisher(std::mutex& mutex, std::string& state, const std::atomic<bool>& cancelled, const bool& failed)
        : mutex_(mutex), state_(state), cancelled_(cancelled), failed_(failed) {}
    ~JobFinisher() {
        const std::lock_guard lock(mutex_);
        if (cancelled_) state_ = "cancelled";
        else if (failed_ || std::uncaught_exceptions() > exceptions_) state_ = "failed";
        else state_ = "done";
    }
    JobFinisher(const JobFinisher&) = delete;
    JobFinisher& operator=(const JobFinisher&) = delete;
    JobFinisher(JobFinisher&&) = delete;
    JobFinisher& operator=(JobFinisher&&) = delete;

private:
    std::mutex& mutex_;
    std::string& state_;
    const std::atomic<bool>& cancelled_;
    const bool& failed_;
    int exceptions_ = std::uncaught_exceptions();
};

}  // namespace

JobManager::JobManager(Calculator& calc, int jobThreads, int cellThreads, int keepMinutes, Logger& log)
    : calc_(calc), keepMinutes_(keepMinutes), log_(log), cells_(cellThreads), runners_(jobThreads) {}

JobManager::~JobManager() {
    std::lock_guard lock(mutex_);
    for (const auto& [id, j] : jobs_) j->cancelled = true;
}

void JobManager::sweep() {
    const TimePoint now = Clock::now();
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const bool finished = it->second->state != "queued" && it->second->state != "running";
        it = finished && now - it->second->created > std::chrono::minutes(keepMinutes_) ? jobs_.erase(it) : std::next(it);
    }
}

json JobManager::start(std::string_view missionId, const json& mission) {
    auto job = std::make_shared<Job>();
    job->id = Secrets::randomToken(12);
    job->missionId = missionId;
    job->created = Clock::now();
    for (LinkKind k : Calculator::kindsFor(mission)) {
        Part part;
        part.link = toString(k);
        part.name = "Coverage grid, " + part.link + " link";
        job->parts.push_back(std::move(part));
    }
    // Fail fast on a mission the calculation would refuse, rather than in the background.
    (void)calc_.contextFor(mission);
    {
        std::lock_guard lock(mutex_);
        sweep();
        jobs_[job->id] = job;
    }
    runners_.submit([this, job, mission] { runJob(*job, mission); });
    std::lock_guard lock(mutex_);
    return toJson(*job);
}

void JobManager::runJob(Job& job, const json& mission) {
    {
        const std::lock_guard lock(mutex_);
        job.state = "running";
    }
    bool failed = false;
    const JobFinisher finisher(mutex_, job.state, job.cancelled, failed);
    for (Part& part : job.parts) {
        if (job.cancelled) break;
        runPart(job, part, mission, failed);
    }
}

void JobManager::runPart(const Job& job, Part& part, const json& mission, bool& failed) {
    {
        const std::lock_guard lock(mutex_);
        part.state = "running";
    }
    try {
        PartProgress progress(mutex_, part.progress, job.cancelled);
        json grid = calc_.coverage(mission, linkKindFromString(part.link), progress, cells_);
        const std::lock_guard lock(mutex_);
        part.state = grid.value("cached", false) ? "cached" : "done";
        part.progress = 1.0;
        grid.erase("cached");
        part.grid = std::move(grid);
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): a part contains every failure of its calculation
        // A part is contained: whatever the calculation throws becomes the part's error.
        recordFailure(job, part, e.what(), failed);
    }
}

void JobManager::recordFailure(const Job& job, Part& part, const char* what, bool& failed) {
    const std::lock_guard lock(mutex_);
    part.state = job.cancelled ? "waiting" : "failed";
    part.error = what;
    failed = !job.cancelled;
    if (failed) log_.warn("coverage", "job " + job.id + " part " + part.name + ": " + what);
}

std::shared_ptr<JobManager::Job> JobManager::find(std::string_view missionId, std::string_view jobId) {
    std::lock_guard lock(mutex_);
    const auto it = jobs_.find(jobId);
    // Another mission's job reads as unknown, so ids cannot be probed across missions.
    if (it == jobs_.end() || it->second->missionId != missionId) throw HttpError(404, "no such job");
    return it->second;
}

json JobManager::toJson(const Job& j) const {
    json parts = json::array();
    for (const Part& p : j.parts) {
        json pj = {{"name", p.name}, {"link", p.link}, {"state", p.state}, {"progress", p.progress}};
        if (!p.error.empty()) pj["error"] = p.error;
        parts.push_back(pj);
    }
    return {{"jobId", j.id}, {"state", j.state}, {"parts", parts}, {"createdAt", isoUtc(j.created)}};
}

json JobManager::status(const std::string& missionId, const std::string& jobId) {
    auto j = find(missionId, jobId);
    std::lock_guard lock(mutex_);
    return toJson(*j);
}

void JobManager::cancel(const std::string& missionId, const std::string& jobId) {
    auto j = find(missionId, jobId);
    j->cancelled = true;
    std::lock_guard lock(mutex_);
    if (j->state == "queued") j->state = "cancelled";
}

json JobManager::grid(const std::string& missionId, const std::string& jobId, const std::string& link) {
    auto j = find(missionId, jobId);
    std::lock_guard lock(mutex_);
    for (const Part& p : j->parts) {
        if (p.link != link) continue;
        if (p.state == "done" || p.state == "cached") return p.grid;
        if (p.state == "failed") throw HttpError(500, "coverage failed: " + p.error);
        throw HttpError(409, "the " + link + " part is " + p.state, {{"progress", p.progress}});
    }
    throw HttpError(404, "this job does not compute the " + link + " link");
}

std::size_t JobManager::running() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [id, j] : jobs_) {
        if (j->state == "running" || j->state == "queued") ++n;
    }
    return n;
}

}  // namespace rfmap
