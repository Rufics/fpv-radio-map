#include "region_definition.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <set>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxIdLength = 64;
constexpr std::size_t kMaxNameLength = 120;
constexpr int kMaxZoom = 22;

bool validLonLat(const json& position) {
    if (!position.is_array() || position.size() < 2 || !position[0].is_number() || !position[1].is_number()) {
        return false;
    }
    const double lon = position[0].get<double>();
    const double lat = position[1].get<double>();
    return lon >= -180.0 && lon <= 180.0 && lat >= -90.0 && lat <= 90.0;
}

/// A polygon's rings, each closed and of at least four valid positions.
bool validPolygon(const json& rings) {
    if (!rings.is_array() || rings.empty()) return false;
    return std::ranges::all_of(rings, [](const json& ring) {
        return ring.is_array() && ring.size() >= 4 && std::ranges::all_of(ring, validLonLat) && ring.front() == ring.back();
    });
}

/// A glyph range as the fonts directory names its files: "0-255", "256-511", ...
bool validRange(const std::string& range) {
    const auto dash = range.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == range.size()) return false;
    return std::ranges::all_of(range, [](char c) { return (c >= '0' && c <= '9') || c == '-'; }) &&
           range.find('-', dash + 1) == std::string::npos;
}

std::vector<std::string> stringList(const json& entry, const char* key) {
    std::vector<std::string> out;
    if (!entry.contains(key)) return out;
    const json& list = entry.at(key);
    if (!list.is_array() || !std::ranges::all_of(list, [](const json& v) { return v.is_string(); })) {
        throw RegionError(std::format("{} must be a list of strings", key));
    }
    for (const json& v : list) out.push_back(v.get<std::string>());
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

json readJsonFile(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw RegionError("cannot read " + file.string());
    try {
        return json::parse(in, nullptr, true, true);
    } catch (const json::exception& e) {
        throw RegionError(std::format("{} is not JSON: {}", file.string(), e.what()));
    }
}

}  // namespace

GeoBox::GeoBox(double minLon, double minLat, double maxLon, double maxLat)
    : minLon_(minLon), minLat_(minLat), maxLon_(maxLon), maxLat_(maxLat) {}

std::optional<GeoBox> GeoBox::fromJson(const json& value) {
    if (!value.is_array() || value.size() != 4) return std::nullopt;
    if (!std::ranges::all_of(value, [](const json& v) { return v.is_number(); })) return std::nullopt;
    const GeoBox box(value[0].get<double>(), value[1].get<double>(), value[2].get<double>(), value[3].get<double>());
    const bool lonOk = box.minLon_ >= -180.0 && box.minLon_ < box.maxLon_ && box.maxLon_ <= 180.0;
    const bool latOk = box.minLat_ >= -90.0 && box.minLat_ < box.maxLat_ && box.maxLat_ <= 90.0;
    if (!lonOk || !latOk) return std::nullopt;
    return box;
}

json GeoBox::toJson() const { return json::array({minLon_, minLat_, maxLon_, maxLat_}); }

std::string GeoBox::toArgument() const { return std::format("{},{},{},{}", minLon_, minLat_, maxLon_, maxLat_); }

RegionArea RegionArea::fromGeoJson(const json& geojson) {
    json polygons = json::array();
    const std::string type = geojson.is_object() ? geojson.value("type", "") : "";
    if (type == "FeatureCollection" && geojson.contains("features") && geojson.at("features").is_array()) {
        for (const json& feature : geojson.at("features")) {
            if (feature.is_object() && feature.contains("geometry")) collect(feature.at("geometry"), polygons);
        }
    } else if (type == "Feature" && geojson.contains("geometry")) {
        collect(geojson.at("geometry"), polygons);
    } else {
        collect(geojson, polygons);
    }
    if (polygons.empty()) throw RegionError("the boundary holds no Polygon or MultiPolygon");
    return {boxAround(polygons), json{{"type", "MultiPolygon"}, {"coordinates", polygons}}};
}

void RegionArea::collect(const json& geometry, json& polygons) {
    if (!geometry.is_object() || !geometry.contains("coordinates")) return;
    const std::string type = geometry.value("type", "");
    const json& coordinates = geometry.at("coordinates");
    if (type == "Polygon") {
        if (!validPolygon(coordinates)) throw RegionError("a Polygon of the boundary has invalid rings");
        polygons.push_back(coordinates);
        return;
    }
    if (type != "MultiPolygon" || !coordinates.is_array()) return;
    for (const json& polygon : coordinates) {
        if (!validPolygon(polygon)) throw RegionError("a MultiPolygon of the boundary has invalid rings");
        polygons.push_back(polygon);
    }
}

GeoBox RegionArea::boxAround(const json& polygons) {
    double minLon = std::numeric_limits<double>::max();
    double minLat = minLon;
    double maxLon = std::numeric_limits<double>::lowest();
    double maxLat = maxLon;
    for (const json& polygon : polygons) {
        // The outer ring bounds the polygon; holes lie inside it.
        for (const json& position : polygon.front()) {
            minLon = std::min(minLon, position[0].get<double>());
            maxLon = std::max(maxLon, position[0].get<double>());
            minLat = std::min(minLat, position[1].get<double>());
            maxLat = std::max(maxLat, position[1].get<double>());
        }
    }
    const auto box = GeoBox::fromJson(json::array({minLon, minLat, maxLon, maxLat}));
    if (!box) throw RegionError("the boundary encloses no area");
    return *box;
}

RegionDefinition::RegionDefinition(const json& entry, const fs::path& base)
    : id_(entry.is_object() ? entry.value("id", "") : ""), area_(readArea(entry, base)) {
    if (!validId(id_)) throw RegionError(std::format("'{}' is no region id (lower-case letters, digits and dashes)", id_));
    name_ = entry.value("name", id_);
    if (name_.empty() || name_.size() > kMaxNameLength) {
        throw RegionError(std::format("name must be 1 to {} characters", kMaxNameLength));
    }
    readOptions(entry);
}

bool RegionDefinition::validId(std::string_view id) {
    if (id.empty() || id.size() > kMaxIdLength || id.front() == '-' || id.back() == '-') return false;
    return std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

RegionArea RegionDefinition::readArea(const json& entry, const fs::path& base) {
    if (!entry.is_object()) throw RegionError("a region must be a JSON object");
    const int given = static_cast<int>(entry.contains("bbox")) + static_cast<int>(entry.contains("boundary")) +
                      static_cast<int>(entry.contains("geometry"));
    if (given != 1) throw RegionError("give exactly one of bbox, boundary (a GeoJSON file) or geometry");
    if (entry.contains("bbox")) {
        const auto box = GeoBox::fromJson(entry.at("bbox"));
        if (!box) throw RegionError("bbox must be [minLon, minLat, maxLon, maxLat] within the world");
        return RegionArea(*box);
    }
    if (entry.contains("geometry")) return RegionArea::fromGeoJson(entry.at("geometry"));
    if (!entry.at("boundary").is_string()) throw RegionError("boundary must be a file name");
    return RegionArea::fromGeoJson(readJsonFile(boundaryFile(entry.at("boundary").get<std::string>(), base)));
}

fs::path RegionDefinition::boundaryFile(const std::string& name, const fs::path& base) {
    const fs::path relative = fs::path(name).lexically_normal();
    if (name.empty() || relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
        throw RegionError("boundary must be a path inside the directory of the definitions file");
    }
    return base / relative;
}

void RegionDefinition::readOptions(const json& entry) {
    if (entry.contains("maxzoom")) {
        const json& z = entry.at("maxzoom");
        if (!z.is_number_integer() || z.get<int>() < 0 || z.get<int>() > kMaxZoom) {
            throw RegionError(std::format("maxzoom must be a whole number from 0 to {}", kMaxZoom));
        }
        maxZoom_ = z.get<int>();
    }
    layers_ = stringList(entry, "layers");
    for (const std::string& layer : layers_) {
        if (std::ranges::find(kLayers, layer) == kLayers.end()) throw RegionError("unknown layer '" + layer + "'");
    }
    glyphRanges_ = stringList(entry, "glyphRanges");
    if (!std::ranges::all_of(glyphRanges_, validRange)) throw RegionError("glyphRanges must look like \"0-255\"");
}

std::string RegionDefinition::canonical() const {
    const auto& polygon = area_.polygon();
    json doc = {{"id", id_},
                {"name", name_},
                {"bbox", area_.box().toJson()},
                {"polygon", polygon.value_or(json())},
                {"maxzoom", maxZoom_ ? json(*maxZoom_) : json()},
                {"layers", layers_},
                {"glyphRanges", glyphRanges_}};
    return doc.dump();
}

std::vector<RegionDefinition> RegionDefinitions::load(const fs::path& file, const Skipped& skipped) {
    std::vector<RegionDefinition> out;
    std::error_code ec;
    if (!fs::exists(file, ec)) return out;
    const json doc = readJsonFile(file);
    if (!doc.is_object() || !doc.contains("regions") || !doc.at("regions").is_array()) {
        throw RegionError(file.string() + " must hold {\"regions\": [...]}");
    }
    std::set<std::string, std::less<>> taken;
    for (const json& entry : doc.at("regions")) {
        const std::string label = entry.is_object() ? entry.value("id", "?") : "?";
        try {
            RegionDefinition definition(entry, file.parent_path());
            if (!taken.insert(definition.id()).second) throw RegionError("the id is used by an earlier region");
            out.push_back(std::move(definition));
        } catch (const RegionError& e) {
            skipped(label, e.what());
        } catch (const json::exception& e) {
            skipped(label, e.what());
        }
    }
    std::ranges::sort(out, {}, &RegionDefinition::id);
    return out;
}

}  // namespace rfmap
