// rfmap: the FPV Radio Map command-line client. The commands live in rfmap::cli::Cli; this is the
// process around it: the build's version, the RFMAP_* environment and the standard streams.

#include "cli.hpp"

#include <unistd.h>  // environ

#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef RFMAP_CLI_VERSION
constexpr const char* kVersion = RFMAP_CLI_VERSION;
#else
constexpr const char* kVersion = "0.0.0";
#endif

int main(int argc, char** argv) {
    std::map<std::string, std::string, std::less<>> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string kv = *e;
        if (const auto eq = kv.find('='); eq != std::string::npos && kv.starts_with("RFMAP_")) {
            env[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    const std::vector<std::string> words(argv + 1, argv + argc);
    rfmap::cli::Cli cli(kVersion, {std::cin, std::cout, std::cerr, std::move(env),
                                   [](std::chrono::milliseconds wait) { std::this_thread::sleep_for(wait); }});
    return cli.run(words);
}
