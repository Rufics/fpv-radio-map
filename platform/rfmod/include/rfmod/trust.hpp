#pragma once

#include "rfmod/crypto.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

struct TrustedKey {
    PublicKey key;
    std::string label;
    bool builtIn = false;  ///< The owner key compiled into the host; cannot be removed.

    /// Modules signed with the owner key run in the host process; every other key's modules
    /// run in an isolated worker.
    [[nodiscard]] bool firstParty() const { return builtIn; }
};

/// The trust store file could not be written.
class TrustStoreWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The signing keys this host accepts. The owner key is compiled in; the admin adds or
/// removes the others, which are kept in a JSON file.
class TrustStore {
public:
    /// `ownerKeyText` is the RFMAP_OWNER_PUBKEY build value. `storePath` holds admin keys.
    TrustStore(std::string_view ownerKeyText, std::filesystem::path storePath);

    [[nodiscard]] const std::vector<TrustedKey>& keys() const { return keys_; }
    [[nodiscard]] std::optional<TrustedKey> find(std::string_view keyId) const;

    /// Adds an admin key. Throws std::invalid_argument for a bad key or a duplicate.
    const TrustedKey& add(const PublicKey& key, std::string label);
    /// Removes an admin key. Throws std::invalid_argument for the owner key or an unknown id.
    void remove(const std::string& keyId);

private:
    void load();
    void save() const;

    std::filesystem::path storePath_;
    std::vector<TrustedKey> keys_;
};

}  // namespace rfmod
