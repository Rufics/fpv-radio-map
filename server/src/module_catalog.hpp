#pragma once

#include "builtins.hpp"
#include "common.hpp"
#include "security.hpp"

#include <rfmod/registry.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rfmap {

/// What the API tells about modules: the list of built-in and installed ones, and the files a
/// module publishes (its UI bundle and options schema). Installing is not an API matter at
/// all; the rfmod CLI does it on the machine.
class ModuleCatalog {
public:
    ModuleCatalog(const BuiltinModules& builtins, const rfmod::ModuleRegistry& registry, const BundleGrants& grants);

    /// GET /modules. Alpha bundles are for the admin only: others see the module without its
    /// `ui`, the admin's listing carries a short-lived signed grant in the bundle URL, because a
    /// browser imports bundles without headers.
    [[nodiscard]] json list(bool admin) const;

    struct PublicFile {
        std::filesystem::path path;
        std::string contentType;
        bool alpha = false;  ///< needs the admin or a grant
    };
    /// A module's public file, or nothing: lib/ and anything outside ui/ or the options schema
    /// is never served, whatever the path says.
    [[nodiscard]] std::optional<PublicFile> file(const std::string& id, const std::string& path) const;
    [[nodiscard]] bool grantValid(const std::string& id, std::string_view grant, TimePoint now) const;

    /// Where GET /modules/{id}/{file} serves a package-relative path.
    [[nodiscard]] static std::string fileUrl(const std::string& id, const std::string& path);
    [[nodiscard]] static std::string contentType(std::string_view extension);

    static constexpr std::chrono::hours kGrantLifetime{12};

private:
    void describeInstalled(json& m, bool admin) const;

    const BuiltinModules& builtins_;
    const rfmod::ModuleRegistry& registry_;
    const BundleGrants& grants_;
};

}  // namespace rfmap
