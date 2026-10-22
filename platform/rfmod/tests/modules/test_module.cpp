// A module for the platform tests. TEST_ID and TEST_VERSION are set per build so the tests can
// make several distinct modules from one source.
#include <rfmod/module.hpp>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

#ifndef TEST_ID
#error "TEST_ID required"
#endif

class TestModule final : public rfmod::Module {
public:
    const char* manifest() const override {
        static const std::string m = rfmod::json{{"id", TEST_ID}, {"version", TEST_VERSION}}.dump();
        return m.c_str();
    }

    void init(const rfmod::HostClient&, const rfmod::json& options) override {
        if (options.value("failInit", false)) throw std::runtime_error("asked to fail");
    }

    rfmod::json invoke(const std::string& method, const rfmod::json& request) override {
        if (method == "echo") return request;
        if (method == "ping-host") return host().call("test.ping", request);
        if (method == "crash") std::abort();
        if (method == "sleep") {
            std::this_thread::sleep_for(std::chrono::milliseconds(request.value("ms", 0)));
            return {{"slept", true}};
        }
        if (method == "allocate") {
            std::vector<char> big(static_cast<std::size_t>(request.value("mb", 0)) << 20, 1);
            return {{"bytes", big.size()}};
        }
        if (method == "bad") throw rfmod::BadRequest("nope");
        if (method == "probe") return probe();
        throw rfmod::UnknownMethod(method);
    }

    /// What a hostile module could try: each attempt and whether the sandbox let it through.
    static rfmod::json probe() {
        const bool readsEtc = std::ifstream("/etc/passwd").good();
        const bool readsOwnDir = std::ifstream("manifest.json").good();
        const int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) close(sock);
        const pid_t child = fork();
        if (child == 0) _exit(0);
        if (child > 0) waitpid(child, nullptr, 0);
        // The host is the same user, so only the sandbox can refuse the signal.
        const bool signalsOthers = kill(getppid(), 0) == 0;
        return {{"readsEtc", readsEtc}, {"readsOwnDir", readsOwnDir}, {"opensSocket", sock >= 0},
                {"forks", child > 0}, {"signalsOthers", signalsOthers}, {"writesHere", std::ofstream("probe.tmp").good()}};
    }

    rfmod::json selftest() override {
        const bool ok = !SELFTEST_FAILS;
        return {{"ok", ok}, {"checks", {{{"name", "echo"}, {"ok", ok}}}}};
    }
};

RFMOD_DEFINE_MODULE(TestModule)
