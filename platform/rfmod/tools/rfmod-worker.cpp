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

#include <cerrno>
#include <cstdio>
#include <string>

namespace {

using json = nlohmann::json;
constexpr int kFd = 3;

std::string inbox;

void sendLine(const json& msg) {
    const std::string line = msg.dump() + "\n";
    std::size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = write(kFd, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(1);
        off += static_cast<std::size_t>(n);
    }
}

json readLine() {
    for (;;) {
        const auto nl = inbox.find('\n');
        if (nl != std::string::npos) {
            const std::string line = inbox.substr(0, nl);
            inbox.erase(0, nl + 1);
            return json::parse(line);
        }
        char buf[65536];
        const ssize_t n = read(kFd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(0);  // host went away
        inbox.append(buf, static_cast<std::size_t>(n));
    }
}

long long nextCallback = 1;

int hostCall(void*, const char* method, const char* request, const char** response) {
    thread_local std::string last;
    const long long id = nextCallback++;
    json req;
    try {
        req = request && *request ? json::parse(request) : json::object();
    } catch (const std::exception& e) {
        last = e.what();
        *response = last.c_str();
        return RFMOD_ERR_BAD_REQUEST;
    }
    sendLine({{"t", "cb"}, {"id", id}, {"method", method ? method : ""}, {"request", req}});
    for (;;) {
        const json msg = readLine();
        if (msg.value("t", "") == "cbr" && msg.value("id", 0LL) == id) {
            if (msg.value("ok", false)) {
                last = msg.value("result", json()).dump();
                *response = last.c_str();
                return RFMOD_OK;
            }
            last = msg.value("error", std::string("host call failed"));
            *response = last.c_str();
            return RFMOD_ERR_HOST_CALL;
        }
        // The host sends nothing else while it waits for us; anything else is a protocol error.
        _exit(2);
    }
}

void hostLog(void*, int level, const char* message) {
    sendLine({{"t", "log"}, {"level", level}, {"message", message ? message : ""}});
}

/// The worker after its arguments are checked. Returns the exit code.
int run(const char* library) {

    // Hardening that does not need privileges. Each result is reported to the host as it is.
    const bool noNewPrivs = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
    // A new user+network namespace leaves the module with only a loopback device and no route out.
    // Where the kernel or container forbids unprivileged namespaces this fails and we say so.
    const bool netIsolated = unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0;

    const json init = readLine();
    auto fail = [&](const std::string& message) {
        sendLine({{"t", "ready"}, {"status", 1}, {"message", message}});
        return 1;
    };
    if (init.value("t", "") != "init") return fail("expected init");

    void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return fail(std::string("dlopen: ") + dlerror());
    auto entry = reinterpret_cast<rfmod_get_module_fn>(dlsym(handle, RFMOD_ENTRY_SYMBOL));
    if (!entry) return fail("library does not export " RFMOD_ENTRY_SYMBOL);
    const rfmod_module* api = entry();
    if (!api || api->abi_version != RFMOD_ABI_VERSION) return fail("library speaks a different rfmod ABI");

    static rfmod_host host{RFMOD_ABI_VERSION, nullptr, &hostCall, &hostLog};
    const std::string options = init.value("options", json::object()).dump();
    if (api->init(&host, options.c_str()) != RFMOD_OK) return fail("module init returned an error");

    sendLine({{"t", "ready"},
              {"status", 0},
              {"manifest", std::string(api->manifest_json())},
              {"sandbox", {{"noNewPrivileges", noNewPrivs}, {"networkIsolated", netIsolated}}}});

    for (;;) {
        const json msg = readLine();
        const std::string t = msg.value("t", "");
        if (t == "shutdown") break;
        if (t != "invoke") continue;
        char* response = nullptr;
        const std::string method = msg.value("method", "");
        const std::string request = msg.value("request", json::object()).dump();
        const int rc = api->invoke(method.c_str(), request.c_str(), &response);
        json body;
        if (response) {
            try {
                body = json::parse(response);
            } catch (const std::exception&) {
                body = {{"error", {{"code", "internal"}, {"message", "module returned invalid JSON"}}}};
            }
            api->free_string(response);
        }
        sendLine({{"t", "res"}, {"id", msg.value("id", 0LL)}, {"status", rc}, {"body", body}});
    }
    api->shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        (void)std::fprintf(stderr, "rfmod-worker is started by the rfmap host, not by hand\n");
        return 64;
    }
    // stderr goes to the host's log, so an unexpected failure is reported there instead of aborting.
    try {
        return run(argv[1]);
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "rfmod-worker: %s\n", e.what());
        return 70;
    }
}
