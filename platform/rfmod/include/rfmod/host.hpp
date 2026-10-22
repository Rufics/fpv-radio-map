#pragma once

#include "rfmod/confined_root.hpp"
#include "rfmod/manifest.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rfmod {

using json = nlohmann::json;

/// An error a module call ended with, as {code, message}.
class ModuleError : public std::runtime_error {
public:
    ModuleError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    [[nodiscard]] const std::string& code() const { return code_; }

private:
    std::string code_;
};

/// What the host offers modules: "link.evaluate", "profile.get", "geodesy.*", "weather.point",
/// "library.get". Implemented by the server; the module platform only forwards calls.
class IHostServices {
public:
    virtual ~IHostServices() = default;
    /// Throws on failure; the message reaches the module as a host_call error.
    virtual json call(const std::string& moduleId, const std::string& method, const json& request) = 0;
    virtual void log(const std::string& moduleId, int level, const std::string& message) = 0;
};

/// A loaded module, in process or in a worker. Instances are shared: whoever holds one may call
/// it after the registry shut it down, and gets a ModuleError "unavailable" instead of a crash.
class IModuleInstance {
public:
    virtual ~IModuleInstance() = default;
    [[nodiscard]] virtual const Manifest& manifest() const = 0;
    [[nodiscard]] virtual bool isolated() const = 0;
    /// Throws ModuleError.
    virtual json invoke(const std::string& method, const json& request) = 0;
    virtual void shutdown() = 0;
};

/// The host's ceiling on what a module may ask for in its manifest `limits`, and on what a
/// worker may send back. The effective limit is the smaller of the manifest's and the policy's.
struct ModulePolicy {
    unsigned maxMemoryMb = 2048;
    unsigned maxTimeoutMs = 60000;
    std::size_t maxReplyBytes = 16u << 20;  ///< one protocol reply from a worker
    std::size_t maxLogBytes = 1u << 20;     ///< unfinished stderr line buffered for the log
    /// A module that is not first-party loads only when its worker confined itself: file
    /// system (Landlock), system calls (seccomp) and network (seccomp or a namespace).
    bool requireSandbox = true;

    [[nodiscard]] Limits effective(const Limits& requested) const;
};

/// What the worker sandbox actually achieved; reported, never assumed.
struct SandboxInfo {
    bool noNewPrivileges = false;
    bool networkIsolated = false;
    bool filesystemIsolated = false;  ///< Landlock: the module reads its own directory, nothing else
    bool syscallFilter = false;       ///< seccomp: no sockets, processes, signals, ptrace, mounts
    unsigned memoryLimitMb = 0;

    /// Every confinement a third-party module needs under ModulePolicy::requireSandbox.
    [[nodiscard]] bool complete() const { return noNewPrivileges && networkIsolated && filesystemIsolated && syscallFilter; }
    [[nodiscard]] std::string describe() const;
};

/// Loads the shared library into this process. Only for packages signed with the owner key.
/// The library must lie inside `trustedDir`; anything else throws PathEscapeError before dlopen.
class InProcessModule final : public IModuleInstance {
public:
    InProcessModule(Manifest manifest, const ConfinedRoot& trustedDir, const std::filesystem::path& library,
                    IHostServices& services, const json& options);
    ~InProcessModule() override;

    [[nodiscard]] const Manifest& manifest() const override { return manifest_; }
    [[nodiscard]] bool isolated() const override { return false; }
    json invoke(const std::string& method, const json& request) override;
    void shutdown() override;

private:
    struct Bridge;
    struct Library;
    Manifest manifest_;
    std::unique_ptr<Library> library_;
    const struct rfmod_module* api_ = nullptr;
    std::unique_ptr<Bridge> bridge_;
    std::mutex mutex_;
    bool down_ = false;
};

/// Runs the module in a separate rfmod-worker process with resource limits, talking
/// newline-delimited JSON over a socket. A crash or timeout kills only the worker.
class IsolatedModule final : public IModuleInstance {
public:
    /// `library` lies under `moduleDir`, the only directory the worker keeps read access to.
    IsolatedModule(Manifest manifest, std::filesystem::path moduleDir, std::filesystem::path library,
                   std::filesystem::path workerExecutable, IHostServices& services, json options,
                   const ModulePolicy& policy);
    ~IsolatedModule() override;

    [[nodiscard]] const Manifest& manifest() const override { return manifest_; }
    [[nodiscard]] bool isolated() const override { return true; }
    json invoke(const std::string& method, const json& request) override;
    void shutdown() override;

    [[nodiscard]] const SandboxInfo& sandbox() const { return sandbox_; }
    /// The limits the worker runs under: the manifest's, capped by the policy.
    [[nodiscard]] const Limits& limits() const { return limits_; }

private:
    class Turn;
    void start();
    void stop(bool kill);
    void send(const json& message) const;
    /// The worker's "ready" message, checked: the library matches the manifest and the sandbox
    /// is as complete as the policy asks.
    void acceptReady(const json& ready);
    /// Reads worker messages until one of `type` (and `id`, except for "ready") arrives,
    /// answering callbacks on the way.
    json waitFor(const std::string& type, long long id, long long deadlineMs);
    /// Handles the complete lines in outBuffer_; returns the awaited message if one of them is it.
    std::optional<json> handleBufferedMessages(std::string_view type, long long id);
    /// Runs a host call the worker asked for and sends the result back.
    void answerCallback(const json& msg);
    /// Forwards the complete lines the worker wrote to stderr to the host log.
    void logBufferedStderr();
    /// Waits up to a second (and never past the deadline) for worker output and buffers it.
    void readWorkerOutput(long long deadlineMs);
    /// Appends what the worker wrote on `fd` to the matching buffer, within the policy's limits.
    void bufferOutput(std::size_t stream, const char* data, std::size_t size);

    Manifest manifest_;
    std::filesystem::path moduleDir_;
    std::filesystem::path library_;
    std::filesystem::path worker_;
    IHostServices& services_;
    json options_;
    ModulePolicy policy_;
    Limits limits_;
    SandboxInfo sandbox_;
    /// One exchange with the worker at a time: mutex_ guards only busy_, so the pipe I/O never
    /// runs with a lock held and the next caller sleeps on idle_ until the worker is free.
    std::mutex mutex_;
    std::condition_variable idle_;
    bool busy_ = false;
    bool down_ = false;
    int pid_ = -1;
    int toWorker_ = -1;
    int fromWorker_ = -1;
    int errFromWorker_ = -1;
    std::string outBuffer_;
    std::string errBuffer_;
    long long nextId_ = 1;
};

}  // namespace rfmod
