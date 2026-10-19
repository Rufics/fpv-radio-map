#pragma once

// C++ helper for module authors. Implement rfmod::Module and add RFMOD_DEFINE_MODULE(YourClass)
// in one .cpp file; the macro exports the C ABI entry point. Exceptions never cross the ABI:
// they are turned into JSON error objects here.

#include "rfmod/abi.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <new>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace rfmod {

using json = nlohmann::json;

class HostError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UnknownMethod : public std::runtime_error {
public:
    explicit UnknownMethod(const std::string& method) : std::runtime_error("unknown method: " + method) {}
};

class BadRequest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Module::host() was called before the host initialised the module.
class ModuleNotInitialized : public std::logic_error {
public:
    ModuleNotInitialized() : std::logic_error("module used before init") {}
};

/// Calls into the host (link evaluation, terrain profiles, geodesy, weather, library).
class HostClient {
public:
    explicit HostClient(const rfmod_host* host) : host_(host) {}

    [[nodiscard]] json call(const std::string& method, const json& request) const {
        const char* response = nullptr;
        const std::string body = request.dump();
        if (const int rc = host_->call(host_->context, method.c_str(), body.c_str(), &response);
            rc != RFMOD_OK || response == nullptr) {
            throw HostError("host call failed: " + method + (response ? std::string(": ") + response : ""));
        }
        return json::parse(response);
    }

    void log(int level, const std::string& message) const { log(level, message.c_str()); }
    void log(int level, const char* message) const noexcept { host_->log(host_->context, level, message); }

private:
    const rfmod_host* host_;
};

class Module {
public:
    virtual ~Module() = default;
    [[nodiscard]] virtual const char* manifest() const = 0;
    virtual void init(const HostClient& host, const json& options) { (void)host; (void)options; }
    [[nodiscard]] virtual json invoke(const std::string& method, const json& request) = 0;
    /// Run by the server on upload, in a worker, before anything is installed. Return
    /// {"ok": bool, "checks": [{"name", "ok", "detail"}]}. Check your maths against known cases here.
    [[nodiscard]] virtual json selftest() { return {{"ok", true}, {"checks", json::array()}}; }
    virtual void shutdown() {
        // Nothing to release by default; modules that hold resources override this.
    }

protected:
    [[nodiscard]] const HostClient& host() const {
        if (!host_) throw ModuleNotInitialized();
        return *host_;
    }

private:
    friend struct ModuleAdapter;
    std::unique_ptr<HostClient> host_;
};

/// Glue between the C ABI and a Module instance. Used by RFMOD_DEFINE_MODULE.
struct ModuleAdapter {
    static Module*& instance() {
        static Module* m = nullptr;
        return m;
    }

    /// Strings lent to the host, keyed by the pointer it holds, until it hands each back to
    /// freeString. Each string lives on its own, so its characters never move.
    struct Lent {
        std::mutex mutex;
        std::unordered_map<const char*, std::unique_ptr<std::string>> strings;
    };
    static Lent& lent() {
        static Lent l;
        return l;
    }

    /// Lends `s` to the host; the host hands it back to freeString, never to free().
    /// Null if memory runs out, so no exception crosses the ABI.
    static char* copy(std::string s) noexcept {
        try {
            auto owned = std::make_unique<std::string>(std::move(s));
            char* out = owned->data();
            Lent& l = lent();
            const std::lock_guard lock(l.mutex);
            l.strings.emplace(out, std::move(owned));
            return out;
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    /// A JSON error object lent to the host, or null if it cannot be built.
    static char* error(const char* code, const char* message) noexcept {
        try {
            // Invalid UTF-8 in the message is replaced, not thrown on: this runs while reporting an error.
            return copy(json{{"error", {{"code", code}, {"message", message}}}}.dump(-1, ' ', false,
                                                                                    json::error_handler_t::replace));
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }

    /// Hands an error to the host and returns `status`. With `response`, the error goes back as a
    /// JSON error object; without, it goes to the host log (if the module is initialised).
    static int report(char** response, int status, const char* code, const char* message) noexcept {
        if (response) {
            *response = error(code, message);
        } else if (const auto& host = instance()->host_) {
            host->log(RFMOD_LOG_ERROR, message);
        }
        return status;
    }

    /// Runs `body` (which returns an ABI status) so that no exception crosses the C ABI: every
    /// exception becomes a status, reported as report() describes.
    template <typename Body>
    static int guarded(char** response, Body&& body) noexcept {
        try {
            return std::forward<Body>(body)();
        } catch (const UnknownMethod& e) {
            return report(response, RFMOD_ERR_UNKNOWN_METHOD, "unknown_method", e.what());
        } catch (const BadRequest& e) {
            return report(response, RFMOD_ERR_BAD_REQUEST, "bad_request", e.what());
        } catch (const json::exception& e) {
            return report(response, RFMOD_ERR_BAD_REQUEST, "bad_request", e.what());
        } catch (const HostError& e) {
            return report(response, RFMOD_ERR_HOST_CALL, "host_call", e.what());
        } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): C ABI boundary, every other module error is internal
            return report(response, RFMOD_ERR_INTERNAL, "internal", e.what());
        } catch (...) {  // NOSONAR(cpp:S2738): C ABI boundary, no exception may unwind into the host
            return report(response, RFMOD_ERR_INTERNAL, "internal", "unknown exception");
        }
    }

    static const char* manifestJson() { return instance()->manifest(); }

    static int init(const rfmod_host* host, const char* options) noexcept {
        if (!host || host->abi_version != RFMOD_ABI_VERSION) return RFMOD_ERR_BAD_REQUEST;
        const int status = guarded(nullptr, [host, options] {
            instance()->host_ = std::make_unique<HostClient>(host);
            instance()->init(*instance()->host_, options && *options ? json::parse(options) : json::object());
            return static_cast<int>(RFMOD_OK);
        });
        // Whatever made init fail (the reason went to the host log), the module is not usable.
        return status == RFMOD_OK ? RFMOD_OK : RFMOD_ERR_INTERNAL;
    }

    static int invoke(const char* method, const char* request, char** response) noexcept {
        *response = nullptr;
        return guarded(response, [method, request, response] {
            const json req = request && *request ? json::parse(request) : json::object();
            const std::string name = method ? method : "";
            *response = copy((name == "rfmod.selftest" ? instance()->selftest() : instance()->invoke(name, req)).dump());
            return static_cast<int>(RFMOD_OK);
        });
    }

    static void freeString(const char* s) {
        // Releases the string copy() lent; the map entry owns it.
        Lent& l = lent();
        const std::lock_guard lock(l.mutex);
        l.strings.erase(s);
    }

    /// The host is unloading the module: a failing shutdown is logged to the host, never thrown.
    static void shutdown() noexcept {
        (void)guarded(nullptr, [] {
            instance()->shutdown();
            return static_cast<int>(RFMOD_OK);
        });
    }
};

}  // namespace rfmod

#define RFMOD_DEFINE_MODULE(ClassName)                                                              \
    extern "C" RFMOD_EXPORT const rfmod_module* rfmod_get_module(void) {                          \
        static ClassName instance;                                                                  \
        static const rfmod_module api = {RFMOD_ABI_VERSION,          &rfmod::ModuleAdapter::manifestJson, \
                                         &rfmod::ModuleAdapter::init, &rfmod::ModuleAdapter::invoke, \
                                         &rfmod::ModuleAdapter::freeString,                         \
                                         &rfmod::ModuleAdapter::shutdown};                           \
        rfmod::ModuleAdapter::instance() = &instance;                                               \
        return &api;                                                                                \
    }
