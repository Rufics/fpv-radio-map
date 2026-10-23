// rfmod: the module developer and operator tool. The commands live in rfmod::Cli; this is the
// process around it: the build's version and owner key, where rfmod-worker is, and the streams.

#include "rfmod/cli.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The build passes the project version and owner key; a stand-alone build of this file falls back.
#ifdef RFMAP_HOST_VERSION
constexpr const char* kHostVersion = RFMAP_HOST_VERSION;
#else
constexpr const char* kHostVersion = "0.1.0";
#endif
#ifdef RFMAP_OWNER_PUBKEY_TEXT
constexpr const char* kOwnerKey = RFMAP_OWNER_PUBKEY_TEXT;
#else
constexpr const char* kOwnerKey = "";
#endif

/// rfmod-worker next to this binary (a build tree), else where `cmake --install` puts it
/// relative to this binary (bin/rfmod and lib/rfmap/rfmod-worker).
fs::path workerExecutable() {
    const fs::path self = fs::canonical("/proc/self/exe").parent_path();
    for (const fs::path& candidate : {self / "rfmod-worker", self.parent_path() / "lib" / "rfmap" / "rfmod-worker"}) {
        if (fs::is_regular_file(candidate)) return candidate;
    }
    return self / "rfmod-worker";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> words(argv + 1, argv + argc);
    const rfmod::Cli cli({kHostVersion, kOwnerKey, workerExecutable()}, std::cout, std::cerr);
    return cli.run(words);
}
