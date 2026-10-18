#include "rfmod/host.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

namespace rfmod {
namespace {

constexpr long long kStartupTimeoutMs = 10000;
constexpr int kProtocolFd = 3;  // the worker's end of the socket, see tools/rfmod-worker.cpp

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Runs between fork and exec, so it takes only string literals: their length is known at compile
// time and nothing has to scan memory.
template <std::size_t N>
[[noreturn]] void childFail(int errPipe, const char (&what)[N]) {
    (void)!write(errPipe, what, N - 1);
    _exit(127);
}

}  // namespace

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

IsolatedModule::IsolatedModule(Manifest manifest, std::filesystem::path library, std::filesystem::path workerExecutable,
                               IHostServices& services, json options)
    : manifest_(std::move(manifest)),
      library_(std::move(library)),
      worker_(std::move(workerExecutable)),
      services_(services),
      options_(std::move(options)) {
    const Turn turn(*this);
    start();
}

IsolatedModule::~IsolatedModule() {
    try {
        shutdown();
    } catch (...) {
    }
}

void IsolatedModule::start() {
    int sock[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sock) != 0) {
        throw ModuleError("load_failed", std::string("socketpair: ") + std::strerror(errno));
    }
    int err[2];
    if (pipe2(err, O_CLOEXEC) != 0) {
        close(sock[0]);
        close(sock[1]);
        throw ModuleError("load_failed", std::string("pipe: ") + std::strerror(errno));
    }

    // Everything the child needs is prepared before fork: after fork only async-signal-safe calls.
    const std::string workerPath = worker_.string();
    const std::string libraryPath = library_.string();
    const std::string dir = library_.parent_path().string();
    const std::string memory = std::to_string(manifest_.limits.memoryMb);
    std::vector<char*> argv = {const_cast<char*>(workerPath.c_str()), const_cast<char*>(libraryPath.c_str()),
                               const_cast<char*>(memory.c_str()), nullptr};
    char* envp[] = {const_cast<char*>("LC_ALL=C"), nullptr};
    const rlim_t memBytes = static_cast<rlim_t>(manifest_.limits.memoryMb) << 20;

    const pid_t pid = fork();
    if (pid < 0) {
        close(sock[0]);
        close(sock[1]);
        close(err[0]);
        close(err[1]);
        throw ModuleError("load_failed", std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0 || dup2(devnull, 0) < 0 || dup2(err[1], 1) < 0 || dup2(err[1], 2) < 0 ||
            dup2(sock[1], kProtocolFd) < 0) {
            childFail(err[1], "rfmod: cannot set up worker descriptors\n");
        }
        // Close everything else the host had open (database sockets, listening sockets, ...).
        close_range(kProtocolFd + 1, ~0u, 0);
        const rlimit as{memBytes, memBytes};
        const rlimit zero{0, 0};
        const rlimit files{32, 32};
        if (setrlimit(RLIMIT_AS, &as) != 0 || setrlimit(RLIMIT_CORE, &zero) != 0 ||
            setrlimit(RLIMIT_FSIZE, &zero) != 0 || setrlimit(RLIMIT_NOFILE, &files) != 0) {
            childFail(2, "rfmod: cannot apply resource limits\n");
        }
        if (chdir(dir.c_str()) != 0) childFail(2, "rfmod: cannot enter module directory\n");
        execve(argv[0], argv.data(), envp);
        childFail(2, "rfmod: cannot start rfmod-worker\n");
    }

    close(sock[1]);
    close(err[1]);
    pid_ = pid;
    toWorker_ = sock[0];
    fromWorker_ = sock[0];
    errFromWorker_ = err[0];
    outBuffer_.clear();
    errBuffer_.clear();

    try {
        send({{"t", "init"}, {"options", options_}});
        const json ready = waitFor("ready", 0, nowMs() + kStartupTimeoutMs);
        if (ready.value("status", 1) != 0) {
            throw ModuleError("init_failed", ready.value("message", std::string("module init failed")));
        }
        const json embedded = json::parse(ready.value("manifest", std::string("{}")));
        if (embedded.value("id", "") != manifest_.id || embedded.value("version", "") != manifest_.version.str()) {
            throw ModuleError("load_failed", "library manifest does not match manifest.json");
        }
        const json sb = ready.value("sandbox", json::object());
        sandbox_.noNewPrivileges = sb.value("noNewPrivileges", false);
        sandbox_.networkIsolated = sb.value("networkIsolated", false);
        sandbox_.memoryLimitMb = manifest_.limits.memoryMb;
    } catch (...) {
        stop(true);
        throw;
    }
}

void IsolatedModule::stop(bool kill) {
    if (pid_ <= 0) return;
    if (kill) {
        ::kill(pid_, SIGKILL);
    } else {
        // Give the worker a moment to shut down cleanly, then make sure it is gone.
        const long long deadline = nowMs() + 2000;
        int status = 0;
        while (waitpid(pid_, &status, WNOHANG) == 0) {
            if (nowMs() > deadline) {
                ::kill(pid_, SIGKILL);
                break;
            }
            pollfd none{};
            poll(&none, 0, 20);
        }
    }
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = -1;
    if (toWorker_ >= 0) close(toWorker_);
    if (errFromWorker_ >= 0) close(errFromWorker_);
    toWorker_ = fromWorker_ = errFromWorker_ = -1;
}

void IsolatedModule::send(const json& message) {
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
    } catch (const std::exception& e) {
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
        } catch (const std::exception&) {
            throw ModuleError("protocol", manifest_.id + ": worker sent a malformed message");
        }
        if (const std::string t = msg.value("t", ""); t == "log") {
            services_.log(manifest_.id, msg.value("level", 1), msg.value("message", ""));
        } else if (t == "cb") {
            answerCallback(msg);
        } else if (t == type && (type == "ready" || msg.value("id", -1LL) == id)) {
            return msg;
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

void IsolatedModule::readWorkerOutput(long long deadlineMs) {
    const long long left = deadlineMs - nowMs();
    if (left <= 0) {
        stop(true);
        throw ModuleError("timeout", manifest_.id + " did not answer within " +
                                         std::to_string(manifest_.limits.timeoutMs) + " ms; worker stopped");
    }
    std::array<pollfd, 2> fds{{{fromWorker_, POLLIN, 0}, {errFromWorker_, POLLIN, 0}}};
    if (const int rc = poll(fds.data(), fds.size(), static_cast<int>(std::min<long long>(left, 1000))); rc < 0 && errno == EINTR) {
        return;
    }
    std::array<char, 65536> buf{};
    for (std::size_t i = 0; i < fds.size(); ++i) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (const ssize_t n = read(fds[i].fd, buf.data(), buf.size()); n > 0) {
            (i == 0 ? outBuffer_ : errBuffer_).append(buf.data(), static_cast<std::size_t>(n));
            if (outBuffer_.size() > (256u << 20)) throw ModuleError("protocol", manifest_.id + ": reply too large");
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
    if (pid_ <= 0) start();  // restart after a crash or timeout
    const long long id = nextId_++;
    send({{"t", "invoke"}, {"id", id}, {"method", method}, {"request", request}});
    const json res = waitFor("res", id, nowMs() + manifest_.limits.timeoutMs);
    const json body = res.value("body", json());
    if (res.value("status", 3) != 0) {
        const json err = body.is_object() && body.contains("error") ? body.at("error") : json::object();
        throw ModuleError(err.value("code", "internal"), err.value("message", std::string("module error")));
    }
    return body;
}

void IsolatedModule::shutdown() {
    const Turn turn(*this);
    if (pid_ <= 0) return;
    try {
        send({{"t", "shutdown"}});
    } catch (...) {
    }
    stop(false);
}

}  // namespace rfmod
