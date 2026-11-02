#include "world_tiles.hpp"

#include "region_definition.hpp"

#include <algorithm>
#include <format>
#include <fstream>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

bool isPlainFile(const fs::path& file) {
    std::error_code ec;
    return fs::symlink_status(file, ec).type() == fs::file_type::regular;
}

bool isPlainDirectory(const fs::path& dir) {
    std::error_code ec;
    return fs::symlink_status(dir, ec).type() == fs::file_type::directory;
}

/// The directories directly in `dir`, sorted so the fingerprint does not depend on the listing order.
std::vector<fs::path> plainDirectories(const fs::path& dir) {
    std::vector<fs::path> out;
    if (!isPlainDirectory(dir)) return out;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (isPlainDirectory(entry.path())) out.push_back(entry.path());
    }
    std::ranges::sort(out);
    return out;
}

}  // namespace

std::vector<fs::path> WorldTiles::plainFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    if (!isPlainDirectory(dir)) return out;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (isPlainFile(entry.path())) out.push_back(entry.path());
    }
    std::ranges::sort(out);
    return out;
}

std::optional<fs::path> WorldTiles::plainFile(std::string_view name) const {
    fs::path file = dir_ / name;
    if (!isPlainFile(file)) return std::nullopt;
    return file;
}

std::optional<fs::path> WorldTiles::layer(std::string_view layer) const {
    return plainFile(std::string(layer) + ".pmtiles");
}

json WorldTiles::style() const {
    const auto file = plainFile(kStyleFile);
    if (!file) throw RegionError(std::format("no {} in {}", kStyleFile, dir_.string()));
    std::ifstream in(*file);
    try {
        return json::parse(in);
    } catch (const json::exception& e) {
        throw RegionError(std::format("{} is not JSON: {}", file->string(), e.what()));
    }
}

std::vector<std::string> WorldTiles::fontstacks() const {
    std::vector<std::string> out;
    for (const fs::path& dir : plainDirectories(dir_ / kFontsDir)) out.push_back(dir.filename().string());
    return out;
}

std::vector<fs::path> WorldTiles::glyphs(const std::string& fontstack, const std::vector<std::string>& ranges) const {
    // A fontstack names a directory: one name, never a path.
    if (fontstack.empty() || fontstack.find('/') != std::string::npos || fontstack.starts_with('.')) return {};
    std::vector<fs::path> out;
    for (const fs::path& file : plainFiles(dir_ / kFontsDir / fontstack)) {
        if (file.extension() != ".pbf") continue;
        if (ranges.empty() || std::ranges::find(ranges, file.stem().string()) != ranges.end()) out.push_back(file);
    }
    return out;
}

std::vector<fs::path> WorldTiles::sprites() const {
    std::vector<fs::path> out;
    for (const fs::path& file : plainFiles(dir_ / kSpritesDir)) {
        const std::string ext = file.extension().string();
        if (ext == ".json" || ext == ".png") out.push_back(file);
    }
    return out;
}

std::optional<fs::path> WorldTiles::dem() const { return plainFile(kDemFile); }

std::string WorldTiles::stamp(const fs::path& file) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    if (ec) return std::format("{}\t-\n", file.filename().string());
    const auto size = isPlainFile(file) ? fs::file_size(file, ec) : 0;
    return std::format("{}\t{}\t{}\n", file.filename().string(), size, mtime);
}

std::string WorldTiles::fingerprint(const std::vector<std::string>& layers) const {
    std::string text;
    for (const std::string& name : layers) {
        if (const auto file = layer(name)) text += stamp(*file);
    }
    text += stamp(dir_ / kStyleFile);
    // A directory's time changes when a file is added or removed in it, a file's when it is
    // written again in place: the fonts directory for its fontstacks, each fontstack's
    // directory and glyph files for its ranges.
    text += stamp(dir_ / kFontsDir);
    for (const fs::path& fontstack : plainDirectories(dir_ / kFontsDir)) {
        text += stamp(fontstack);
        for (const fs::path& glyphs : plainFiles(fontstack)) text += stamp(glyphs);
    }
    text += stamp(dir_ / kSpritesDir);
    for (const fs::path& file : sprites()) text += stamp(file);
    text += stamp(dir_ / kDemFile);
    return text;
}

}  // namespace rfmap
