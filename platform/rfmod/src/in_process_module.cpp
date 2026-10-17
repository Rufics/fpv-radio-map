#include "rfmod/abi.h"
#include "rfmod/host.hpp"

#include <dlfcn.h>

namespace rfmod {

struct InProcessModule::Bridge {
    std::string moduleId;
    IHostServices* services;
    rfmod_host host;

    static int call(void* ctx, const char* method, const char* request, const char** response) {
        // Valid until the next call on this thread, as abi.h promises.
        thread_local std::string last;
        auto* self = static_cast<Bridge*>(ctx);
        try {
            const json req = request && *request ? json::parse(request) : json::object();
            last = self->services->call(self->moduleId, method ? method : "", req).dump();
            *response = last.c_str();
            return RFMOD_OK;
        } catch (const std::exception& e) {
            last = e.what();
            *response = last.c_str();
            return RFMOD_ERR_HOST_CALL;
        }
    }

    static void log(void* ctx, int level, const char* message) {
        auto* self = static_cast<Bridge*>(ctx);
        try {
            self->services->log(self->moduleId, level, message ? message : "");
        } catch (...) {
        }
    }
};

namespace {

json takeResponse(const rfmod_module* api, char* response) {
    if (!response) return json();
    std::string text(response);
    api->free_string(response);
    return json::parse(text);
}

}  // namespace

InProcessModule::InProcessModule(Manifest manifest, const std::filesystem::path& library, IHostServices& services,
                                 const json& options)
    : manifest_(std::move(manifest)), bridge_(std::make_unique<Bridge>()) {
    handle_ = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) throw ModuleError("load_failed", std::string("dlopen: ") + dlerror());
    auto entry = reinterpret_cast<rfmod_get_module_fn>(dlsym(handle_, RFMOD_ENTRY_SYMBOL));
    if (!entry) {
        dlclose(handle_);
        throw ModuleError("load_failed", "library does not export " RFMOD_ENTRY_SYMBOL);
    }
    api_ = entry();
    if (!api_ || api_->abi_version != RFMOD_ABI_VERSION) {
        dlclose(handle_);
        throw ModuleError("load_failed", "library speaks a different rfmod ABI");
    }
    const json embedded = json::parse(api_->manifest_json());
    if (embedded.value("id", "") != manifest_.id || embedded.value("version", "") != manifest_.version.str()) {
        dlclose(handle_);
        throw ModuleError("load_failed", "library manifest does not match manifest.json");
    }
    bridge_->moduleId = manifest_.id;
    bridge_->services = &services;
    bridge_->host = rfmod_host{RFMOD_ABI_VERSION, bridge_.get(), &Bridge::call, &Bridge::log};
    const std::string opts = options.dump();
    if (api_->init(&bridge_->host, opts.c_str()) != RFMOD_OK) {
        dlclose(handle_);
        throw ModuleError("init_failed", "module init returned an error");
    }
}

InProcessModule::~InProcessModule() {
    shutdown();
    if (handle_) dlclose(handle_);
}

json InProcessModule::invoke(const std::string& method, const json& request) {
    std::lock_guard lock(mutex_);
    if (down_) throw ModuleError("unavailable", manifest_.id + " is shut down");
    char* response = nullptr;
    const std::string body = request.dump();
    const int rc = api_->invoke(method.c_str(), body.c_str(), &response);
    json out = takeResponse(api_, response);
    if (rc != RFMOD_OK) {
        const json err = out.is_object() && out.contains("error") ? out.at("error") : json::object();
        throw ModuleError(err.value("code", "internal"), err.value("message", "module error " + std::to_string(rc)));
    }
    return out;
}

void InProcessModule::shutdown() {
    std::lock_guard lock(mutex_);
    if (down_ || !api_) return;
    down_ = true;
    api_->shutdown();
}

}  // namespace rfmod
