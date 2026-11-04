#pragma once

// A small world directory for the offline region tests, and a reader for the packages built
// from it.

#include "common.hpp"
#include "test_support.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace rfmap::test {

/// A tiles-builder output in miniature: three layers, a style with two fonts, glyphs in two
/// ranges, a sprite in 1x and 2x and a DEM for the preview.
class TestWorld {
public:
    explicit TestWorld(std::filesystem::path dir) : dir_(std::move(dir)) {
        std::filesystem::remove_all(dir_);
        for (const char* layer : {"basemap", "hillshade", "terrain"}) write(std::string(layer) + ".pmtiles", std::string("PMTiles ") + layer);
        write("style-dark.json", style().dump());
        write("fonts/Noto Sans Regular/0-255.pbf", "regular latin");
        write("fonts/Noto Sans Regular/1024-1279.pbf", "regular cyrillic");
        write("fonts/Noto Sans Medium/0-255.pbf", "medium latin");
        write("fonts/Noto Sans Medium/1024-1279.pbf", "medium cyrillic");
        write("fonts/Unused Font/0-255.pbf", "unused");
        for (const char* sprite : {"sprite.json", "sprite.png", "sprite@2x.json", "sprite@2x.png"}) write(std::string("sprites/") + sprite, sprite);
        writeCog(dir_ / "dem.cog.tif", 35.0, 48.8, 0.01, 60, 40, [](int c, int r) { return 100.0 + 5.0 * c + 3.0 * r; });
    }

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

    void write(const std::string& name, const std::string& bytes) const {
        const std::filesystem::path file = dir_ / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    }

    /// The style as tiles-builder writes it, with a sprite.
    static json style() {
        const auto label = [](const char* id, const char* font) {
            return json{{"id", id}, {"type", "symbol"}, {"source", "openmaptiles"}, {"layout", {{"text-font", {font}}}}};
        };
        return {{"version", 8},
                {"glyphs", "/tiles/fonts/{fontstack}/{range}.pbf"},
                {"sprite", "/tiles/sprites/sprite"},
                {"sources",
                 {{"openmaptiles", {{"type", "vector"}, {"url", "pmtiles:///tiles/basemap.pmtiles"}}},
                  {"satellite", {{"type", "raster"}, {"url", "pmtiles:///tiles/satellite.pmtiles"}}},
                  {"hillshade", {{"type", "raster"}, {"url", "pmtiles:///tiles/hillshade.pmtiles"}}}}},
                {"layers",
                 {{{"id", "background"}, {"type", "background"}},
                  {{"id", "satellite"}, {"type", "raster"}, {"source", "satellite"}},
                  {{"id", "hillshade"}, {"type", "raster"}, {"source", "hillshade"}},
                  label("place", "Noto Sans Regular"),
                  label("road", "Noto Sans Medium")}}};
    }

private:
    std::filesystem::path dir_;
};

/// The entries of a zip in order, with their bytes.
class ZipContents {
public:
    explicit ZipContents(const std::filesystem::path& zip) {
        const std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), &archive_read_free);
        archive_read_support_format_zip(a.get());
        if (archive_read_open_filename(a.get(), zip.c_str(), 1 << 16) != ARCHIVE_OK) {
            throw std::runtime_error("cannot open " + zip.string());
        }
        archive_entry* entry = nullptr;
        while (archive_read_next_header(a.get(), &entry) == ARCHIVE_OK) {
            std::string bytes(static_cast<std::size_t>(archive_entry_size(entry)), '\0');
            if (!bytes.empty()) archive_read_data(a.get(), bytes.data(), bytes.size());
            names.emplace_back(archive_entry_pathname(entry));
            files[names.back()] = bytes;
        }
    }

    std::vector<std::string> names;
    std::map<std::string, std::string> files;
};

}  // namespace rfmap::test
