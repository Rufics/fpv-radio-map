#include "region_package.hpp"

#include "region_archive.hpp"
#include "security.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <tuple>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr int kRecordLayout = 1;
constexpr std::string_view kTempPrefix = ".tmp-";
constexpr std::string_view kWorkPrefix = ".work-";

bool isHex64(const std::string& text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void renameInto(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) throw RegionError(std::format("cannot move {} to {}: {}", from.string(), to.string(), ec.message()));
}

/// Removes the work directory and temporary files of a build when it ends, finished or not.
class BuildScratch {
public:
    BuildScratch(const RegionPackageStore& store, const std::string& id)
        : paths_{store.workDir(id), store.tempArchive(id), store.tempPreview(id)} {
        clear();
        fs::create_directories(paths_[0]);
    }
    BuildScratch(const BuildScratch&) = delete;
    BuildScratch& operator=(const BuildScratch&) = delete;
    ~BuildScratch() { clear(); }

private:
    void clear() const noexcept {
        std::error_code ec;
        for (const fs::path& p : paths_) fs::remove_all(p, ec);
    }
    std::array<fs::path, 3> paths_;
};

}  // namespace

// ---- record ------------------------------------------------------------------------------

json RegionPackage::toRecord() const {
    return {{"layout", kRecordLayout},
            {"id", id},
            {"name", name},
            {"bbox", bbox.toJson()},
            {"maxzoom", maxZoom.has_value() ? json(*maxZoom) : json()},
            {"layers", layers},
            {"size", size},
            {"version", version},
            {"sha256", sha256},
            {"builtAt", builtAt},
            {"fingerprint", fingerprint},
            {"preview", preview}};
}

std::optional<RegionPackage> RegionPackage::fromRecord(const json& record) {
    try {
        if (!record.is_object() || record.value("layout", 0) != kRecordLayout) return std::nullopt;
        const auto box = GeoBox::fromJson(record.at("bbox"));
        if (!box) return std::nullopt;
        RegionPackage p;
        p.id = record.at("id").get<std::string>();
        p.name = record.at("name").get<std::string>();
        p.bbox = *box;
        if (record.at("maxzoom").is_number_integer()) p.maxZoom = record.at("maxzoom").get<int>();
        p.layers = record.at("layers").get<std::vector<std::string>>();
        p.size = record.at("size").get<std::uintmax_t>();
        p.version = record.at("version").get<std::string>();
        p.sha256 = record.at("sha256").get<std::string>();
        p.builtAt = record.at("builtAt").get<std::string>();
        p.fingerprint = record.at("fingerprint").get<std::string>();
        p.preview = record.at("preview").get<bool>();
        if (!RegionDefinition::validId(p.id) || !isHex64(p.version) || !isHex64(p.sha256)) return std::nullopt;
        return p;
    } catch (const json::exception&) {
        return std::nullopt;  // a damaged record: the region is built again
    }
}

// ---- store -------------------------------------------------------------------------------

std::optional<RegionPackage> RegionPackageStore::load(const std::string& id) const {
    std::ifstream in(record(id));
    if (!in) return std::nullopt;
    json doc;
    try {
        doc = json::parse(in);
    } catch (const json::exception&) {
        return std::nullopt;
    }
    auto package = RegionPackage::fromRecord(doc);
    if (!package || package->id != id) return std::nullopt;
    std::error_code ec;
    if (fs::file_size(archive(*package), ec) != package->size || ec) return std::nullopt;
    if (package->preview && !fs::is_regular_file(preview(*package), ec)) package->preview = false;
    return package;
}

void RegionPackageStore::commit(const RegionPackage& package) const {
    renameInto(tempArchive(package.id), archive(package));
    if (package.preview) renameInto(tempPreview(package.id), preview(package));
    const fs::path temp = dir_ / std::format("{}{}.json", kTempPrefix, package.id);
    {
        std::ofstream out(temp, std::ios::trunc);
        out << package.toRecord().dump(2) << '\n';
        if (!out) throw RegionError("cannot write " + temp.string());
    }
    renameInto(temp, record(package.id));
}

fs::path RegionPackageStore::archive(const RegionPackage& package) const {
    return dir_ / std::format("{}-{}.zip", package.id, package.version);
}

fs::path RegionPackageStore::preview(const RegionPackage& package) const {
    return dir_ / std::format("{}-{}.png", package.id, package.version);
}

fs::path RegionPackageStore::workDir(const std::string& id) const { return dir_ / std::format("{}{}", kWorkPrefix, id); }

fs::path RegionPackageStore::tempArchive(const std::string& id) const {
    return dir_ / std::format("{}{}.zip", kTempPrefix, id);
}

fs::path RegionPackageStore::tempPreview(const std::string& id) const {
    return dir_ / std::format("{}{}.png", kTempPrefix, id);
}

std::set<std::string, std::less<>> RegionPackageStore::inUse(const std::set<std::string, std::less<>>& current) const {
    std::set<std::string, std::less<>> names{lockFile().filename().string()};
    for (const std::string& id : current) {
        const auto package = load(id);
        if (!package) continue;
        names.insert(record(id).filename().string());
        names.insert(archive(*package).filename().string());
        if (package->preview) names.insert(preview(*package).filename().string());
    }
    return names;
}

std::size_t RegionPackageStore::removeUnused(const std::set<std::string, std::less<>>& current) const {
    const auto keep = inUse(current);
    std::size_t removed = 0;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir_, ec)) {
        const std::string name = entry.path().filename().string();
        const bool ours = name.starts_with(kTempPrefix) || name.starts_with(kWorkPrefix) || name.ends_with(".zip") ||
                          name.ends_with(".png") || name.ends_with(".json");
        if (!ours || keep.contains(name)) continue;
        if (fs::remove_all(entry.path(), ec) > 0) ++removed;
    }
    return removed;
}

// ---- style -------------------------------------------------------------------------------

std::set<std::string, std::less<>> RegionStyle::rewriteSources(json& style, const std::vector<std::string>& layers) {
    std::set<std::string, std::less<>> dropped;
    if (!style.contains("sources") || !style.at("sources").is_object()) return dropped;
    json& sources = style.at("sources");
    for (auto& [name, source] : sources.items()) {
        const std::string url = source.is_object() ? source.value("url", "") : "";
        if (!url.starts_with("pmtiles://")) continue;
        const std::string layer = fs::path(url.substr(url.rfind('/') + 1)).stem().string();
        if (std::ranges::find(layers, layer) == layers.end()) dropped.insert(name);
        else source["url"] = std::format("pmtiles://tiles/{}.pmtiles", layer);
    }
    for (const std::string& name : dropped) sources.erase(name);
    return dropped;
}

json RegionStyle::rewrite(json style, const RegionPackage& package, bool sprites) {
    const auto dropped = rewriteSources(style, package.layers);
    if (style.contains("layers") && style.at("layers").is_array()) {
        json& layers = style.at("layers");
        layers.erase(std::remove_if(layers.begin(), layers.end(),
                                    [&dropped](const json& l) {
                                        return l.is_object() && dropped.contains(l.value("source", std::string()));
                                    }),
                     layers.end());
    }
    if (std::ranges::find(package.layers, "terrain") != package.layers.end()) {
        style["sources"]["terrain"] = {{"type", "raster-dem"},
                                       {"url", "pmtiles://tiles/terrain.pmtiles"},
                                       {"encoding", "terrarium"},
                                       {"tileSize", 256}};
    }
    style["glyphs"] = "glyphs/{fontstack}/{range}.pbf";
    if (sprites) style["sprite"] = "sprites/sprite";
    else style.erase("sprite");
    style["center"] = {package.bbox.centreLon(), package.bbox.centreLat()};
    style["metadata"]["rfmap:region"] = package.id;
    style["metadata"]["rfmap:note"] = "URLs are relative to the root of the region package";
    return style;
}

std::vector<std::string> RegionStyle::fontstacks(const json& style) {
    std::set<std::string, std::less<>> out;
    if (!style.contains("layers") || !style.at("layers").is_array()) return {};
    for (const json& layer : style.at("layers")) {
        if (!layer.is_object() || !layer.contains("layout") || !layer.at("layout").is_object()) continue;
        const json& fonts = layer.at("layout").value("text-font", json());
        // Only a literal list names its fonts; an expression would have to be evaluated.
        if (!fonts.is_array() || fonts.empty() || !std::ranges::all_of(fonts, [](const json& f) { return f.is_string(); })) {
            continue;
        }
        std::string stack;
        for (const json& f : fonts) stack += (stack.empty() ? "" : ",") + f.get<std::string>();
        out.insert(stack);
    }
    return {out.begin(), out.end()};
}

// ---- builder -----------------------------------------------------------------------------

std::vector<std::string> RegionPackageBuilder::layersOf(const RegionDefinition& region, const WorldTiles& world) {
    std::vector<std::string> out;
    for (const std::string_view layer : RegionDefinition::kLayers) {
        const bool asked = region.layers().empty() || std::ranges::find(region.layers(), layer) != region.layers().end();
        if (asked && world.layer(layer)) out.emplace_back(layer);
    }
    return out;
}

std::vector<RegionPackageBuilder::Entry> RegionPackageBuilder::extractLayers(const RegionDefinition& region,
                                                                             const RegionPackage& package) const {
    const fs::path work = t_.store.workDir(region.id());
    std::optional<fs::path> area;
    if (const auto& polygon = region.area().polygon(); polygon) {
        const fs::path file = work / "area.geojson";
        std::ofstream(file) << polygon->dump();
        area = file;
    }
    fs::create_directories(work / "tiles");
    std::vector<Entry> out;
    for (const std::string& layer : package.layers) {
        const fs::path cut = work / "tiles" / (layer + ".pmtiles");
        const auto source = t_.world.layer(layer);
        if (!source) throw RegionError("the world layer " + layer + " went away during the build");
        t_.pmtiles.extract({*source, cut, region.area().box(), area, region.maxZoom()},
                           work / "pmtiles.log", stop_);
        out.push_back({"tiles/" + layer + ".pmtiles", cut, {}});
    }
    return out;
}

std::vector<RegionPackageBuilder::Entry> RegionPackageBuilder::styleAssets(const RegionDefinition& region,
                                                                           const json& style) const {
    std::vector<Entry> out;
    for (const std::string& stack : RegionStyle::fontstacks(style)) {
        const auto files = t_.world.glyphs(stack, region.glyphRanges());
        if (files.empty()) t_.log.warn("regions", std::format("{}: no glyphs for '{}'", region.id(), stack));
        for (const fs::path& f : files) out.push_back({std::format("glyphs/{}/{}", stack, f.filename().string()), f, {}});
    }
    for (const fs::path& f : t_.world.sprites()) out.push_back({"sprites/" + f.filename().string(), f, {}});
    return out;
}

json RegionPackageBuilder::manifest(RegionPackage& package, const std::vector<Entry>& entries) const {
    json files = json::array();
    std::string list;
    std::uintmax_t total = 0;
    for (const Entry& e : entries) {
        const std::uintmax_t size = e.file.empty() ? e.bytes.size() : fs::file_size(e.file);
        const std::string sha = e.file.empty() ? Secrets::hex(Secrets::sha256(e.bytes)) : Sha256Stream::ofFile(e.file, stop_);
        files.push_back({{"name", e.name}, {"size", size}, {"sha256", sha}});
        list += std::format("{}\t{}\t{}\n", e.name, size, sha);
        total += size;
    }
    package.version = Secrets::hex(Secrets::sha256(std::format("rfmap-region {}\n{}", kFormat, list)));
    return {{"format", "rfmap-region"},
            {"formatVersion", kFormat},
            {"id", package.id},
            {"name", package.name},
            {"version", package.version},
            {"bbox", package.bbox.toJson()},
            {"maxzoom", package.maxZoom.has_value() ? json(*package.maxZoom) : json()},
            {"layers", package.layers},
            {"style", kStyle},
            {"bytes", total},
            {"files", files}};
}

std::pair<std::uintmax_t, std::string> RegionPackageBuilder::writeZip(const json& manifest,
                                                                      const std::vector<Entry>& entries,
                                                                      const std::string& id) const {
    ZipWriter zip(t_.store.tempArchive(id));
    zip.addBytes(std::string(kManifest), manifest.dump(2) + "\n");
    for (const Entry& e : entries) {
        if (e.file.empty()) zip.addBytes(e.name, e.bytes);
        else zip.addFile(e.name, e.file, stop_);
    }
    return zip.close();
}

bool RegionPackageBuilder::writePreview(const RegionPackage& package) const {
    const auto dem = t_.world.dem();
    if (!dem) return false;
    try {
        RegionPreview::render(*dem, package.bbox, t_.store.tempPreview(package.id));
        return true;
    } catch (const RegionError& e) {
        t_.log.warn("regions", std::format("{}: no preview: {}", package.id, e.what()));
        return false;
    }
}

RegionPackage RegionPackageBuilder::build(const RegionDefinition& region, const std::string& fingerprint) const {
    if (stop_.stop_requested()) throw RegionBuildCancelled();
    RegionPackage p;
    p.id = region.id();
    p.name = region.name();
    p.bbox = region.area().box();
    p.maxZoom = region.maxZoom();
    p.layers = layersOf(region, t_.world);
    p.fingerprint = fingerprint;
    if (p.layers.empty()) throw RegionError("the world directory has none of the layers the region asks for");
    const BuildScratch scratch(t_.store, region.id());
    std::vector<Entry> entries = extractLayers(region, p);
    const json style = RegionStyle::rewrite(t_.world.style(), p, !t_.world.sprites().empty());
    entries.push_back({std::string(kStyle), {}, style.dump(2) + "\n"});
    std::ranges::move(styleAssets(region, style), std::back_inserter(entries));
    std::ranges::sort(entries, {}, &Entry::name);
    const json description = manifest(p, entries);
    std::tie(p.size, p.sha256) = writeZip(description, entries, region.id());
    p.builtAt = UtcTime::format(t_.clock.now());
    p.preview = writePreview(p);
    t_.store.commit(p);
    return p;
}

}  // namespace rfmap
