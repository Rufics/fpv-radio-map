#include "rfmod/host.hpp"

#include "worker_exec.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <system_error>

namespace rfmod {
namespace {

constexpr long long kStartupTimeoutMs = 10000;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Limits ModulePolicy::effective(const Limits& requested) const {
    Limits out;
    out.memoryMb = std::max(1u, std::min(requested.memoryMb, maxMemoryMb));
    out.timeoutMs = std::max(1u, std::min(requested.timeoutMs, maxTimeoutMs));
    return out;
}

std::string SandboxInfo::describe() const {
    std::string out;
    const auto add = [&out](const char* name, bool on) {
        if (!out.empty()) out += ", ";
        out += name;
        out += on ? ": yes" : ": no";
    };
    add("no-new-privileges", noNewPrivileges);
    add("network isolated", networkIsolated);
    add("file system confined", filesystemIsolated);
    add("syscall filter", syscallFilter);
    return out;
}

/// Holds the worker for the lifetime of the object; see busy_.
class IsolatedModule::Turn {
public:
    explicit Turn(IsolatedModule& owner) : owner_(owner) {
        std::unique_lock lock(owner_.mutex_);
        owner_.idle_.wait(lock, [this] { return !owner_.busy_; });
        owner_.busy_ = true;
    }
    ~Turn() {
        {
            const std::lock_guard lock(owner_.mutex_);
            owner_.busy_ = false;
        }
        owner_.idle_.notify_one();
    }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    Turn(Turn&&) = delete;
    Turn& operator=(Turn&&) = delete;

private:
    IsolatedModule& owner_;
};

IsolatedModule::IsolatedModule(Manifest manifest, std::filesystem::path moduleDir, std::filesystem::path library,
                               std::filesystem::path workerExecutable, IHostServices& services, json options,
                               const ModulePolicy& policy)
    : manifest_(std::move(manifest)),
      moduleDir_(std::move(moduleDir)),
      library_(std::move(library)),
      worker_(std::move(workerExecutable)),
      services_(services),
      options_(std::move(options)),
      policy_(policy),
      limits_(policy.effective(manifest_.limits)) {
    const Turn turn(*this);
    start();
}

IsolatedModule::~IsolatedModule() {
    try {
        shutdown();
    } catch (const std::system_error& e) {
        // A destructor must not throw, and stop() kills the worker either way: only report it.
        services_.log(manifest_.id, RFMOD_LOG_WARN, std::format("worker shutdown failed: {}", e.what()));
    }
}

void IsolatedModule::start() {
    std::array<int, 2> sock{};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sock.data()) != 0) {
        throw ModuleError("load_failed", std::string("socketpair: ") + std::strerror(errno));
    }
    std::array<int, 2> err{};
    if (pipe2(err.data(), O_CLOEXEC) != 0) {
        close(sock[0]);
        close(sock[1]);
        throw ModuleError("load_failed", std::string("pipe: ") + std::strerror(errno));
    }

    // Everything the child needs is prepared before fork: after fork only async-signal-safe calls.
    // execve takes mutable strings, so argv and envp point into strings this function owns.
    std::string workerPath = worker_.string();
    std::string libraryPath = library_.string();
    const std::string dir = moduleDir_.string();
    std::string memory = std::to_string(limits_.memoryMb);
    std::string locale = "LC_ALL=C";
    const std::array<char*, 4> argv = {workerPath.data(), libraryPath.data(), memory.data(), nullptr};
    const std::array<char*, 2> envp = {locale.data(), nullptr};
    const rlim_t memBytes = static_cast<rlim_t>(limits_.memoryMb) << 20;
    const pid_t hostPid = getpid();

    const pid_t pid = fork();
    if (pid < 0) {
        close(sock[0]);
        close(sock[1]);
        close(err[0]);
        close(err[1]);
        throw ModuleError("load_failed", std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) WorkerExec::run({argv.data(), envp.data(), dir.c_str(), sock[1], err[1], memBytes, hostPid});

    close(sock[1]);
    close(err[1]);
    pid_ = pid;
    toWorker_ = sock[0];
    fromWorker_ = sock[0];
    errFromWorker_ = err[0];
    outBuffer_.clear();
    errBuffer_.clear();

    try {
        send({{"t", "init"}, {"options", options_}, {"requireSandbox", policy_.requireSandbox}});
        acceptReady(waitFor("ready", 0, nowMs() + kStartupTimeoutMs));
    } catch (...) {
        stop(true);
        throw;
    }
}

void IsolatedModule::acceptReady(const json& ready) {
    const json sb = ready.value("sandbox", json::object());
    sandbox_.noNewPrivileges = sb.value("noNewPrivileges", false);
    sandbox_.networkIsolated = sb.value("networkIsolated", false);
    sandbox_.filesystemIsolated = sb.value("filesystemIsolated", false);
    sandbox_.syscallFilter = sb.value("syscallFilter", false);
    sandbox_.memoryLimitMb = limits_.memoryMb;
    // The worker refuses to load the library when the policy requires a sandbox it lacks.
    if (policy_.requireSandbox && !sandbox_.complete()) {
        throw ModuleError("sandbox_incomplete",
                          std::format("{}: the worker could not confine itself ({}); refusing to run an unconfined module",
                                      manifest_.id, sandbox_.describe()));
    }
    if (ready.value("status", 1) != 0) {
        throw ModuleError("init_failed", ready.value("message", std::string("module init failed")));
    }
    if (const json embedded = json::parse(ready.value("manifest", std::string("{}")));
        embedded.value("id", "") != manifest_.id || embedded.value("version", "") != manifest_.version.str()) {
        throw ModuleError("load_failed", "library manifest does not match manifest.json");
    }
}

void IsolatedModule::killWorker() const {
    // The worker leads its own process group (WorkerExec::detachFromHost), so -pid reaches
    // anything it managed to spawn as well; the pid itself covers the instant before it led one.
    ::kill(-pid_, SIGKILL);
    ::kill(pid_, SIGKILL);
}

void IsolatedModule::stop(bool kill) {
    if (pid_ <= 0) return;
    if (kill) {
        killWorker();
    } else {
        // Give the worker a moment to shut down cleanly, then make sure it is gone.
        const long long deadline = nowMs() + 2000;
        int status = 0;
        while (waitpid(pid_, &status, WNOHANG) == 0) {
            if (nowMs() > deadline) {
                killWorker();
                break;
            }
            pollfd none{};
            poll(&none, 0, 20);
        }
        killWorker();  // whatever the group still holds
    }
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = -1;
    if (toWorker_ >= 0) close(toWorker_);
    if (errFromWorker_ >= 0) close(errFromWorker_);
    toWorker_ = -1;
    fromWorker_ = -1;
    errFromWorker_ = -1;
}

void IsolatedModule::send(const json& message) const {
    const std::string line = message.dump() + "\n";
    std::size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::send(toWorker_, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw ModuleError("worker_crashed", manifest_.id + ": worker is not accepting requests");
        off += static_cast<std::size_t>(n);
    }
}

void IsolatedModule::answerCallback(const json& msg) {
    json reply = {{"t", "cbr"}, {"id", msg.value("id", 0LL)}};
    try {
        reply["result"] = services_.call(manifest_.id, msg.value("method", ""), msg.value("request", json::object()));
        reply["ok"] = true;
    } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): host services may throw anything; the module gets it
        reply["ok"] = false;
        reply["error"] = e.what();
    }
    send(reply);
}

std::optional<json> IsolatedModule::handleBufferedMessages(std::string_view type, long long id) {
    for (std::size_t nl = outBuffer_.find('\n'); nl != std::string::npos; nl = outBuffer_.find('\n')) {
        const std::string line = outBuffer_.substr(0, nl);
        outBuffer_.erase(0, nl + 1);
        json msg;
        try {
            msg = json::parse(line);
        } catch (const json::exception&) {
            throw ModuleError("protocol", manifest_.id + ": worker sent a malformed message");
        }
        if (const std::string t = msg.value("t", ""); t == "log") {
            services_.log(manifest_.id, msg.value("level", 1), msg.value("message", ""));
        } else if (t == "cb") {
            answerCallback(msg);
        } else if (t == type && (type == "ready" || msg.value("id", -1LL) == id)) {
            return std::make_optional(std::move(msg));
        } else {
            throw ModuleError("protocol", manifest_.id + ": unexpected worker message " + t);
        }
    }
    return std::nullopt;
}

void IsolatedModule::logBufferedStderr() {
    for (std::size_t nl = errBuffer_.find('\n'); nl != std::string::npos; nl = errBuffer_.find('\n')) {
        services_.log(manifest_.id, 2, "stderr: " + errBuffer_.substr(0, nl));
        errBuffer_.erase(0, nl + 1);
    }
}

void IsolatedModule::bufferOutput(std::size_t stream, const char* data, std::size_t size) {
    if (stream == 0) {
        if (outBuffer_.size() + size > policy_.maxReplyBytes) {
            stop(true);
            throw ModuleError("protocol", std::format("{}: worker reply exceeds {} bytes; worker stopped", manifest_.id,
                                                      policy_.maxReplyBytes));
        }
        outBuffer_.append(data, size);
        return;
    }
    // Log output is best effort: a module that floods stderr loses the flood, not its worker.
    // A line that reaches the limit without ending is logged cut, so the buffer never sticks.
    const std::size_t room = policy_.maxLogBytes > errBuffer_.size() ? policy_.maxLogBytes - errBuffer_.size() : 0;
    errBuffer_.append(data, std::min(size, room));
    if (errBuffer_.size() >= policy_.maxLogBytes && errBuffer_.find('\n') == std::string::npos) {
        services_.log(manifest_.id, 2, "stderr: " + errBuffer_ + " [line cut]");
        errBuffer_.clear();
    }
}

void IsolatedModule::readWorkerOutput(long long deadlineMs) {
    const long long left = deadlineMs - nowMs();
    if (left <= 0) {
        stop(true);
        throw ModuleError("timeout", std::format("{} did not answer within {} ms; worker stopped", manifest_.id,
                                                 limits_.timeoutMs));
    }
    std::array<pollfd, 2> fds{{{fromWorker_, POLLIN, 0}, {errFromWorker_, POLLIN, 0}}};
    if (const int rc = poll(fds.data(), fds.size(), static_cast<int>(std::min<long long>(left, 1000))); rc < 0 && errno == EINTR) {
        return;
    }
    std::array<char, 65536> buf{};
    for (std::size_t i = 0; i < fds.size(); ++i) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (const ssize_t n = read(fds[i].fd, buf.data(), buf.size()); n > 0) {
            bufferOutput(i, buf.data(), static_cast<std::size_t>(n));
        } else if (i == 0) {
            const std::string lastWords = errBuffer_;
            stop(true);
            throw ModuleError("worker_crashed",
                              manifest_.id + ": worker exited" + (lastWords.empty() ? "" : ": " + lastWords));
        }
    }
}

json IsolatedModule::waitFor(const std::string& type, long long id, long long deadlineMs) {
    for (;;) {
        // Handle every complete protocol line already buffered.
        if (auto msg = handleBufferedMessages(type, id)) return std::move(*msg);
        logBufferedStderr();
        readWorkerOutput(deadlineMs);
    }
}

json IsolatedModule::invoke(const std::string& method, const json& request) {
    const Turn turn(*this);
    if (down_) throw ModuleError("unavailable", manifest_.id + " has been shut down");
    if (pid_ <= 0) start();  // restart after a crash or timeout
    const long long id = nextId_++;
    send({{"t", "invoke"}, {"id", id}, {"method", method}, {"request", request}});
    const json res = waitFor("res", id, nowMs() + limits_.timeoutMs);
    const json body = res.value("body", json());
    if (res.value("status", 3) != 0) {
        const json err = body.is_object() && body.contains("error") ? body.at("error") : json::object();
        throw ModuleError(err.value("code", "internal"), err.value("message", std::string("module error")));
    }
    return body;
}

void IsolatedModule::shutdown() {
    const Turn turn(*this);
    down_ = true;
    if (pid_ <= 0) return;
    try {
        send({{"t", "shutdown"}});
    } catch (const ModuleError& e) {
        // A worker that cannot be told to stop is stopped by stop() below.
        services_.log(manifest_.id, RFMOD_LOG_DEBUG, std::format("worker missed the shutdown request: {}", e.what()));
    }
    stop(false);
}

}  // namespace rfmod
