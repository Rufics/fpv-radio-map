#include "region_definition.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <set>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxIdLength = 64;
constexpr std::size_t kMaxNameLength = 120;
constexpr int kMaxZoom = 22;
/// The mean radius of the Earth (IUGG), km.
constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

/// The area a closed ring encloses on the sphere, km², whichever way it runs: the line integral
/// of Chamberlain and Duquette, "Some algorithms for polygons on a sphere" (JPL, 2007).
double ringAreaKm2(const json& ring) {
    const std::size_t n = ring.size() - 1;  // the last position repeats the first
    if (n < 3) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const json& previous = ring[(i + n - 1) % n];
        const json& next = ring[(i + 1) % n];
        const double dLon = (next[0].get<double>() - previous[0].get<double>()) * kRadiansPerDegree;
        sum += dLon * std::sin(ring[i][1].get<double>() * kRadiansPerDegree);
    }
    return std::abs(sum) * kEarthRadiusKm * kEarthRadiusKm / 2.0;
}

/// A position of a ring, in degrees, taken as a point of the plane: drawn areas are small
/// enough for the edges' crossings to be judged so.
struct PlanePoint {
    double lon;
    double lat;
};

using PlaneRing = std::vector<PlanePoint>;

/// The ring's corners, without the closing position.
PlaneRing planeRing(const json& ring) {
    PlaneRing out;
    out.reserve(ring.size() - 1);
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) out.push_back({ring[i][0].get<double>(), ring[i][1].get<double>()});
    return out;
}

/// Which side of the line from `o` through `a` the point `b` lies on: > 0 left, < 0 right, 0 on it.
double turn(const PlanePoint& o, const PlanePoint& a, const PlanePoint& b) {
    return (a.lon - o.lon) * (b.lat - o.lat) - (a.lat - o.lat) * (b.lon - o.lon);
}

/// Whether `p`, on the line through `a` and `b`, lies between them.
bool within(const PlanePoint& a, const PlanePoint& b, const PlanePoint& p) {
    return p.lon >= std::min(a.lon, b.lon) && p.lon <= std::max(a.lon, b.lon) && p.lat >= std::min(a.lat, b.lat) &&
           p.lat <= std::max(a.lat, b.lat);
}

bool opposite(double x, double y) { return (x > 0.0 && y < 0.0) || (x < 0.0 && y > 0.0); }

/// Whether the edges a-b and c-d cross or touch.
bool edgesMeet(const PlanePoint& a, const PlanePoint& b, const PlanePoint& c, const PlanePoint& d) {
    const double ab_c = turn(a, b, c);
    const double ab_d = turn(a, b, d);
    const double cd_a = turn(c, d, a);
    const double cd_b = turn(c, d, b);
    if (opposite(ab_c, ab_d) && opposite(cd_a, cd_b)) return true;
    return (ab_c == 0.0 && within(a, b, c)) || (ab_d == 0.0 && within(a, b, d)) || (cd_a == 0.0 && within(c, d, a)) ||
           (cd_b == 0.0 && within(c, d, b));
}

/// Whether two edges of the ring that do not follow each other cross or touch.
bool crossesItself(const PlaneRing& ring) {
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;  // the last edge follows the first
            if (edgesMeet(ring[i], ring[(i + 1) % n], ring[j], ring[(j + 1) % n])) return true;
        }
    }
    return false;
}

/// Whether an edge of one ring crosses or touches an edge of the other.
bool ringsMeet(const PlaneRing& a, const PlaneRing& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (edgesMeet(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()])) return true;
        }
    }
    return false;
}

/// Whether `p` lies inside the ring (even-odd rule).
bool inside(const PlaneRing& ring, const PlanePoint& p) {
    bool in = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const PlanePoint& a = ring[i];
        const PlanePoint& b = ring[j];
        if ((a.lat > p.lat) != (b.lat > p.lat) && p.lon < (b.lon - a.lon) * (p.lat - a.lat) / (b.lat - a.lat) + a.lon) in = !in;
    }
    return in;
}

/// What is wrong with the shape of one polygon's rings, if anything.
std::optional<std::string> polygonFlaw(const json& polygon) {
    std::vector<PlaneRing> rings;
    for (const json& ring : polygon) rings.push_back(planeRing(ring));
    if (std::ranges::any_of(rings, crossesItself)) return "an outline of the polygon crosses itself";
    for (std::size_t hole = 1; hole < rings.size(); ++hole) {
        if (ringsMeet(rings.front(), rings[hole]) || !inside(rings.front(), rings[hole].front())) {
            return "a hole of the polygon is not inside its outline";
        }
        for (std::size_t other = hole + 1; other < rings.size(); ++other) {
            if (ringsMeet(rings[hole], rings[other]) || inside(rings[hole], rings[other].front()) ||
                inside(rings[other], rings[hole].front())) {
                return "two holes of the polygon overlap";
            }
        }
    }
    return std::nullopt;
}

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

double RegionArea::areaKm2() const {
    if (!polygon_) {
        const double dLon = (box_.maxLon() - box_.minLon()) * kRadiansPerDegree;
        const double band = std::sin(box_.maxLat() * kRadiansPerDegree) - std::sin(box_.minLat() * kRadiansPerDegree);
        return dLon * band * kEarthRadiusKm * kEarthRadiusKm;
    }
    double total = 0.0;
    for (const json& polygon : polygon_->at("coordinates")) {
        double area = ringAreaKm2(polygon.front());
        for (std::size_t hole = 1; hole < polygon.size(); ++hole) area -= ringAreaKm2(polygon[hole]);
        total += std::max(0.0, area);
    }
    return total;
}

std::optional<std::string> RegionArea::flaw() const {
    if (!polygon_) return std::nullopt;
    for (const json& polygon : polygon_->at("coordinates")) {
        if (auto found = polygonFlaw(polygon)) return found;
    }
    return std::nullopt;
}

std::size_t RegionArea::vertices() const {
    if (!polygon_) return 4;
    std::size_t count = 0;
    for (const json& polygon : polygon_->at("coordinates")) {
        for (const json& ring : polygon) count += ring.size() - 1;
    }
    return count;
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
                {"maxzoom", maxZoom_.has_value() ? json(*maxZoom_) : json()},
                {"layers", layers_},
                {"glyphRanges", glyphRanges_}};
    return doc.dump();
}

RegionDefinitions::RegionDefinitions(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw RegionError(std::format("cannot read {}: {}", file.string(), ec.message()));
        }
        return;  // no file: no regions
    }
    const json doc = readJsonFile(file);
    if (!doc.is_object() || !doc.contains("regions") || !doc.at("regions").is_array()) {
        throw RegionError(file.string() + " must hold {\"regions\": [...]}");
    }
    std::set<std::string, std::less<>> taken;
    for (const json& entry : doc.at("regions")) {
        const bool named = entry.is_object() && entry.contains("id") && entry.at("id").is_string();
        std::string label = named ? entry.at("id").get<std::string>() : "?";
        try {
            RegionDefinition definition(entry, file.parent_path());
            if (!taken.insert(definition.id()).second) throw RegionError("the id is used by an earlier region");
            regions_.push_back(std::move(definition));
        } catch (const RegionError& e) {
            skipped_.emplace_back(std::move(label), e.what());
        } catch (const json::exception& e) {
            skipped_.emplace_back(std::move(label), e.what());
        }
    }
    std::ranges::sort(regions_, {}, &RegionDefinition::id);
}

}  // namespace rfmap
