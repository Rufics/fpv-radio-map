#pragma once

#include "rfmod/package.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace rfmod::test {

inline std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

inline SecretKey ownerSecret() { return SecretKey::fromText(readFile(RFMOD_TEST_KEYS "/dev-owner.secret")); }
inline SecretKey thirdPartySecret() { return SecretKey::fromText(readFile(RFMOD_TEST_KEYS "/dev-thirdparty.secret")); }
inline std::string ownerPublicText() { return readFile(RFMOD_TEST_KEYS "/dev-owner.pub"); }

inline nlohmann::json baseManifest(const std::string& id, const std::string& version) {
    return {{"schema", "rfmod/v1"},
            {"id", id},
            {"version", version},
            {"title", "Test module"},
            {"lifecycle", "beta"},
            {"abi", 1},
            {"requires", nlohmann::json::array()},
            {"provides", {"loss.contributor"}},
            {"entry", {{currentPlatform(), "lib/libmod.so"}}}};
}

inline PackageFiles packageWith(const nlohmann::json& manifest, const std::string& libraryPath) {
    PackageFiles f;
    f.files["manifest.json"] = manifest.dump(2);
    f.files["lib/libmod.so"] = readFile(libraryPath);
    return f;
}

}  // namespace rfmod::test
