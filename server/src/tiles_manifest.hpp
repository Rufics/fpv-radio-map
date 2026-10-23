#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rfmap {

/// The manifest.json the tiles-builder writes next to its outputs (DEM, land cover, tiles):
/// which files it made, where each came from and whom to credit. Read once; a manifest that
/// is missing, not JSON or shaped unexpectedly reads as empty, never as an error, since the
/// data files stand on their own.
class TilesManifest {
public:
    /// Reads the manifest next to `dataFile`.
    explicit TilesManifest(const std::filesystem::path& dataFile);

    /// The "source" of the data file in the manifest's "files"; empty when unknown.
    [[nodiscard]] std::string sourceName() const;
    /// Distinct per-file attributions, else the manifest's joined display text; empty without one.
    [[nodiscard]] std::vector<std::string> attributions() const;
    /// File names the manifest lists; nullopt without a readable list.
    [[nodiscard]] std::optional<std::set<std::string, std::less<>>> files() const;

private:
    std::string fileName_;
    std::optional<nlohmann::json> document_;
};

}  // namespace rfmap
