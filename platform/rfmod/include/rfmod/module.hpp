#pragma once

// C++ helper for module authors. Implement rfmod::Module and add RFMOD_DEFINE_MODULE(YourClass)
// in one .cpp file; the macro exports the C ABI entry point. Exceptions never cross the ABI:
// they are turned into JSON error objects here.

#include "rfmod/abi.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

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

/// Calls into the host (link evaluation, terrain profiles, geodesy, weather, library).
class HostClient {
public:
    explicit HostClient(const rfmod_host* host) : host_(host) {}

    [[nodiscard]] json call(const std::string& method, const json& request) const {
        const char* response = nullptr;
        const std::string body = request.dump();
        const int rc = host_->call(host_->context, method.c_str(), body.c_str(), &response);
        if (rc != RFMOD_OK || response == nullptr) {
            throw HostError("host call failed: " + method + (response ? std::string(": ") + response : ""));
        }
        return json::parse(response);
    }

    void log(int level, const std::string& message) const { host_->log(host_->context, level, message.c_str()); }

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
    virtual void shutdown() {}

protected:
    [[nodiscard]] const HostClient& host() const {
        if (!host_) throw std::logic_error("module used before init");
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

    /// A module-owned copy for the host; the host hands it back to freeString, never to free().
    static char* copy(const std::string& s) {
        auto* out = new (std::nothrow) char[s.size() + 1];
        if (out) std::memcpy(out, s.c_str(), s.size() + 1);
        return out;
    }

    static char* error(const char* code, const std::string& message) {
        return copy(json{{"error", {{"code", code}, {"message", message}}}}.dump());
    }

    static const char* manifestJson() { return instance()->manifest(); }

    static int init(const rfmod_host* host, const char* options) {
        try {
            if (!host || host->abi_version != RFMOD_ABI_VERSION) return RFMOD_ERR_BAD_REQUEST;
            instance()->host_ = std::make_unique<HostClient>(host);
            instance()->init(*instance()->host_, options && *options ? json::parse(options) : json::object());
            return RFMOD_OK;
        } catch (...) {
            return RFMOD_ERR_INTERNAL;
        }
    }

    static int invoke(const char* method, const char* request, char** response) {
        *response = nullptr;
        try {
            const json req = request && *request ? json::parse(request) : json::object();
            const std::string name = method ? method : "";
            *response = copy((name == "rfmod.selftest" ? instance()->selftest() : instance()->invoke(name, req)).dump());
            return RFMOD_OK;
        } catch (const UnknownMethod& e) {
            *response = error("unknown_method", e.what());
            return RFMOD_ERR_UNKNOWN_METHOD;
        } catch (const BadRequest& e) {
            *response = error("bad_request", e.what());
            return RFMOD_ERR_BAD_REQUEST;
        } catch (const json::exception& e) {
            *response = error("bad_request", e.what());
            return RFMOD_ERR_BAD_REQUEST;
        } catch (const HostError& e) {
            *response = error("host_call", e.what());
            return RFMOD_ERR_HOST_CALL;
        } catch (const std::exception& e) {
            *response = error("internal", e.what());
            return RFMOD_ERR_INTERNAL;
        } catch (...) {
            *response = error("internal", "unknown exception");
            return RFMOD_ERR_INTERNAL;
        }
    }

    static void freeString(char* s) { delete[] s; }

    static void shutdown() {
        try {
            instance()->shutdown();
        } catch (...) {
        }
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
