#include "rfmod/abi.h"
#include "rfmod/host.hpp"

#include <dlfcn.h>

/// What rfmod_host::context points to for an in-process module: which module is calling and
/// where its calls go. Opaque to the module.
struct rfmod_host_context {
    std::string moduleId;
    rfmod::IHostServices* services = nullptr;
};

namespace rfmod {

struct InProcessModule::Bridge {
    rfmod_host_context context;
    rfmod_host host;

    static int call(rfmod_host_context* self, const char* method, const char* request, const char** response) {
        // Valid until the next call on this thread, as abi.h promises.
        thread_local std::string last;
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

    static void log(rfmod_host_context* self, int level, const char* message) {
        try {
            self->services->log(self->moduleId, level, message ? message : "");
        } catch (...) {
        }
    }
};

/// The module's shared library, open for as long as this object lives.
struct InProcessModule::Library {
    struct Close {
        template <typename Handle>
        void operator()(Handle* handle) const {
            dlclose(handle);
        }
    };
    std::unique_ptr<void, Close> handle;

    explicit Library(const std::filesystem::path& path) : handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (!handle) throw ModuleError("load_failed", std::string("dlopen: ") + dlerror());
    }

    [[nodiscard]] rfmod_get_module_fn entry() const {
        return reinterpret_cast<rfmod_get_module_fn>(dlsym(handle.get(), RFMOD_ENTRY_SYMBOL));
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
    : manifest_(std::move(manifest)), library_(std::make_unique<Library>(library)), bridge_(std::make_unique<Bridge>()) {
    // library_ is a constructed member by now, so every throw below closes the library again.
    const rfmod_get_module_fn entry = library_->entry();
    if (!entry) throw ModuleError("load_failed", std::string("library does not export ") + RFMOD_ENTRY_SYMBOL);
    api_ = entry();
    if (!api_ || api_->abi_version != RFMOD_ABI_VERSION) {
        throw ModuleError("load_failed", "library speaks a different rfmod ABI");
    }
    const json embedded = json::parse(api_->manifest_json());
    if (embedded.value("id", "") != manifest_.id || embedded.value("version", "") != manifest_.version.str()) {
        throw ModuleError("load_failed", "library manifest does not match manifest.json");
    }
    bridge_->context.moduleId = manifest_.id;
    bridge_->context.services = &services;
    bridge_->host = rfmod_host{RFMOD_ABI_VERSION, &bridge_->context, &Bridge::call, &Bridge::log};
    const std::string opts = options.dump();
    if (api_->init(&bridge_->host, opts.c_str()) != RFMOD_OK) {
        throw ModuleError("init_failed", "module init returned an error");
    }
}

InProcessModule::~InProcessModule() {
    shutdown();
    library_.reset();
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
