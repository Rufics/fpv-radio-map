// rfmod-worker: runs one third-party module in its own process.
//
// Started by IsolatedModule with resource limits already applied. Talks newline-delimited JSON
// on fd 3; the module's own stdout and stderr go to the host log. Usage (host only):
//   rfmod-worker <library.so> <memoryMb>

#include "rfmod/abi.h"

#include <nlohmann/json.hpp>

#include <dlfcn.h>
#include <sched.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <iostream>
#include <string>

namespace {

using json = nlohmann::json;

/// The protocol link to the host: newline-delimited JSON on fd 3, both ways.
class Channel {
public:
    /// Sends one message; exits if the host is gone.
    void send(const json& msg) const {
        const std::string line = msg.dump() + "\n";
        std::size_t off = 0;
        while (off < line.size()) {
            const ssize_t n = write(fd_, line.data() + off, line.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(1);
            off += static_cast<std::size_t>(n);
        }
    }

    /// The next message; exits when the host closes the link.
    json receive() {
        for (;;) {
            if (const auto nl = inbox_.find('\n'); nl != std::string::npos) {
                const std::string line = inbox_.substr(0, nl);
                inbox_.erase(0, nl + 1);
                return json::parse(line);
            }
            const ssize_t n = read(fd_, buffer_.data(), buffer_.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(0);  // host went away
            inbox_.append(buffer_.data(), static_cast<std::size_t>(n));
        }
    }

private:
    int fd_ = 3;
    std::string inbox_;
    std::array<char, 65536> buffer_{};
};

}  // namespace

/// What rfmod_host::context points to in the worker: the link to the host and the id of the next
/// host call. Opaque to the module.
struct rfmod_host_context {
    Channel channel;
    long long nextCallback = 1;
};

namespace {

int hostCall(rfmod_host_context* self, const char* method, const char* request, const char** response) {
    thread_local std::string last;
    const long long id = self->nextCallback++;
    json req;
    try {
        req = request && *request ? json::parse(request) : json::object();
    } catch (const json::exception& e) {
        last = e.what();
        *response = last.c_str();
        return RFMOD_ERR_BAD_REQUEST;
    }
    self->channel.send({{"t", "cb"}, {"id", id}, {"method", method ? method : ""}, {"request", req}});
    const json msg = self->channel.receive();
    // The host sends nothing else while it waits for us; anything else is a protocol error.
    if (msg.value("t", "") != "cbr" || msg.value("id", 0LL) != id) _exit(2);
    if (msg.value("ok", false)) {
        last = msg.value("result", json()).dump();
        *response = last.c_str();
        return RFMOD_OK;
    }
    last = msg.value("error", std::string("host call failed"));
    *response = last.c_str();
    return RFMOD_ERR_HOST_CALL;
}

void hostLog(rfmod_host_context* self, int level, const char* message) {
    self->channel.send({{"t", "log"}, {"level", level}, {"message", message ? message : ""}});
}

/// Answers invoke requests until the host asks the module to shut down.
void serve(const rfmod_module* api, Channel& channel) {
    for (;;) {
        const json msg = channel.receive();
        const std::string t = msg.value("t", "");
        if (t == "shutdown") return;
        if (t != "invoke") continue;
        char* response = nullptr;
        const std::string method = msg.value("method", "");
        const std::string request = msg.value("request", json::object()).dump();
        const int rc = api->invoke(method.c_str(), request.c_str(), &response);
        json body;
        if (response) {
            try {
                body = json::parse(response);
            } catch (const json::exception&) {
                body = {{"error", {{"code", "internal"}, {"message", "module returned invalid JSON"}}}};
            }
            api->free_string(response);
        }
        channel.send({{"t", "res"}, {"id", msg.value("id", 0LL)}, {"status", rc}, {"body", body}});
    }
}

/// The worker after its arguments are checked. Returns the exit code.
int run(const char* library) {

    // Hardening that does not need privileges. Each result is reported to the host as it is.
    const bool noNewPrivs = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
    // A new user+network namespace leaves the module with only a loopback device and no route out.
    // Where the kernel or container forbids unprivileged namespaces this fails and we say so.
    const bool netIsolated = unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0;

    // Both outlive every module call: the module keeps the host pointer from init until shutdown.
    rfmod_host_context context;
    const rfmod_host host{RFMOD_ABI_VERSION, &context, &hostCall, &hostLog};

    const json init = context.channel.receive();
    auto fail = [&context](const std::string& message) {
        context.channel.send({{"t", "ready"}, {"status", 1}, {"message", message}});
        return 1;
    };
    if (init.value("t", "") != "init") return fail("expected init");

    void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return fail(std::string("dlopen: ") + dlerror());
    // POSIX guarantees a function symbol's address round-trips through dlsym's void*.
    const auto entry = std::bit_cast<rfmod_get_module_fn>(dlsym(handle, RFMOD_ENTRY_SYMBOL));
    if (!entry) return fail(std::string("library does not export ") + RFMOD_ENTRY_SYMBOL);
    const rfmod_module* api = entry();
    if (!api || api->abi_version != RFMOD_ABI_VERSION) return fail("library speaks a different rfmod ABI");

    if (const std::string options = init.value("options", json::object()).dump();
        api->init(&host, options.c_str()) != RFMOD_OK) {
        return fail("module init returned an error");
    }

    context.channel.send({{"t", "ready"},
                          {"status", 0},
                          {"manifest", std::string(api->manifest_json())},
                          {"sandbox", {{"noNewPrivileges", noNewPrivs}, {"networkIsolated", netIsolated}}}});

    serve(api, context.channel);
    api->shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "rfmod-worker is started by the rfmap host, not by hand\n";
        return 64;
    }
    // stderr goes to the host's log, so a malformed message from the host is reported there
    // instead of aborting.
    try {
        return run(argv[1]);
    } catch (const json::exception& e) {
        std::cerr << "rfmod-worker: " << e.what() << '\n';
        return 70;
    }
}
