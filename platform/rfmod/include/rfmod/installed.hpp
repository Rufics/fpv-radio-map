#pragma once

#include "rfmod/module_store.hpp"
#include "rfmod/package.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfmod {

/// A module in the store whose package verified against the current trust store.
struct InstalledModule {
    Manifest manifest;
    TrustedKey signer;
    std::filesystem::path dir;
    ModuleSettings settings;
};

/// Reads what is installed. Every package is verified again on every read, so a removed key or
/// a changed byte takes a module out of service, and tampered files are restored from the package.
class InstalledReader {
public:
    InstalledReader(const ModuleStore& store, const PackageVerifier& verifier) : store_(store), verifier_(verifier) {}

    /// The module in `dir`; nullopt (with a notice) when its package no longer verifies.
    [[nodiscard]] std::optional<InstalledModule> read(const std::filesystem::path& dir,
                                                      std::vector<std::string>& notices) const;
    /// Every module in the store, by id.
    [[nodiscard]] std::map<std::string, InstalledModule, std::less<>> all(std::vector<std::string>& notices) const;

private:
    const ModuleStore& store_;
    const PackageVerifier& verifier_;
};

}  // namespace rfmod
