#pragma once

#include "rfmod/manifest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

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

/// A loaded module, in process or in a worker.
class IModuleInstance {
public:
    virtual ~IModuleInstance() = default;
    [[nodiscard]] virtual const Manifest& manifest() const = 0;
    [[nodiscard]] virtual bool isolated() const = 0;
    /// Throws ModuleError.
    virtual json invoke(const std::string& method, const json& request) = 0;
    virtual void shutdown() = 0;
};

/// Loads the shared library into this process. Only for packages signed with the owner key.
class InProcessModule final : public IModuleInstance {
public:
    InProcessModule(Manifest manifest, const std::filesystem::path& library, IHostServices& services,
                    const json& options);
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

/// What the worker sandbox actually achieved; reported, never assumed.
struct SandboxInfo {
    bool noNewPrivileges = false;
    bool networkIsolated = false;
    unsigned memoryLimitMb = 0;
};

/// Runs the module in a separate rfmod-worker process with resource limits, talking
/// newline-delimited JSON over pipes. A crash or timeout kills only the worker.
class IsolatedModule final : public IModuleInstance {
public:
    IsolatedModule(Manifest manifest, std::filesystem::path library, std::filesystem::path workerExecutable,
                   IHostServices& services, json options);
    ~IsolatedModule() override;

    [[nodiscard]] const Manifest& manifest() const override { return manifest_; }
    [[nodiscard]] bool isolated() const override { return true; }
    json invoke(const std::string& method, const json& request) override;
    void shutdown() override;

    [[nodiscard]] const SandboxInfo& sandbox() const { return sandbox_; }

private:
    void start();
    void stop(bool kill);
    void send(const json& message) const;
    /// Reads worker messages until `done` returns true, answering callbacks on the way.
    json waitFor(const std::string& type, long long id, long long deadlineMs);

    Manifest manifest_;
    std::filesystem::path library_;
    std::filesystem::path worker_;
    IHostServices& services_;
    json options_;
    SandboxInfo sandbox_;
    std::mutex mutex_;
    int pid_ = -1;
    int toWorker_ = -1;
    int fromWorker_ = -1;
    int errFromWorker_ = -1;
    std::string outBuffer_;
    std::string errBuffer_;
    long long nextId_ = 1;
};

}  // namespace rfmod
