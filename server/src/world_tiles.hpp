#pragma once

// The world-wide map data the offline regions are cut from: one tiles-builder out/ directory
// (regions.worldDir) built for the whole planet, or for as much of it as the operator serves.

#include "common.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// The world directory as it is now. Reading it never follows symbolic links out of it.
///
///   <layer>.pmtiles   basemap, satellite, hillshade, terrain, contours (whichever were built)
///   style-dark.json   the style, its tile URLs pointing at the server
///   fonts/<fontstack>/<range>.pbf   the glyphs
///   sprites/          optional: sprite.json, sprite.png, sprite@2x.json, sprite@2x.png
///   dem.cog.tif       optional: the DEM the previews are drawn from
class WorldTiles {
public:
    static constexpr std::string_view kStyleFile = "style-dark.json";
    static constexpr std::string_view kFontsDir = "fonts";
    static constexpr std::string_view kSpritesDir = "sprites";
    static constexpr std::string_view kDemFile = "dem.cog.tif";

    explicit WorldTiles(std::filesystem::path dir) : dir_(std::move(dir)) {}

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
    /// The .pmtiles file of `layer`; nothing when it was not built.
    [[nodiscard]] std::optional<std::filesystem::path> layer(std::string_view layer) const;
    /// The style; RegionError when it is missing or not JSON.
    [[nodiscard]] json style() const;
    /// The glyph files of `fontstack` whose range is in `ranges` (empty = all), sorted.
    [[nodiscard]] std::vector<std::filesystem::path> glyphs(const std::string& fontstack,
                                                            const std::vector<std::string>& ranges) const;
    /// Every fontstack the fonts directory holds, sorted.
    [[nodiscard]] std::vector<std::string> fontstacks() const;
    /// The sprite files, 1x and 2x, sorted; empty without a sprites directory.
    [[nodiscard]] std::vector<std::filesystem::path> sprites() const;
    [[nodiscard]] std::optional<std::filesystem::path> dem() const;
    /// Name, size and modification time of `file` as a line of a fingerprint.
    [[nodiscard]] static std::string stamp(const std::filesystem::path& file);
    /// Stamps of every file that a package of these layers would read from the world directory.
    [[nodiscard]] std::string fingerprint(const std::vector<std::string>& layers) const;

private:
    /// The regular files directly in `dir`, sorted, links left out.
    [[nodiscard]] static std::vector<std::filesystem::path> plainFiles(const std::filesystem::path& dir);
    [[nodiscard]] std::optional<std::filesystem::path> plainFile(std::string_view name) const;

    std::filesystem::path dir_;
};

}  // namespace rfmap
