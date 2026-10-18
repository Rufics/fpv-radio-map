#include "schemas.hpp"

#include <nlohmann/json-schema.hpp>

#include <format>
#include <fstream>

namespace rfmap {
namespace fs = std::filesystem;
namespace {

constexpr const char* kBase = "https://rfmap.local/schemas/";

json readJson(const fs::path& p) {
    std::ifstream in(p);
    if (!in) throw std::runtime_error("cannot read schema " + p.string());
    return json::parse(in);
}

class Collector final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& ptr, const json& instance, const std::string& message) override {
        basic_error_handler::error(ptr, instance, message);
        problems.push_back({ptr.to_string(), message});
    }
    std::vector<SchemaRegistry::Problem> problems;
};

// The validator's format checks (date-time, uri, ...) use std::regex, which recurses once per
// character: a long enough string in a request body overflows the stack. No valid value of these
// formats comes near the limit, so longer strings are refused before the regex sees them.
constexpr std::size_t kMaxFormattedLength = 2048;

void boundedFormatCheck(const std::string& format, const std::string& value) {
    if (value.size() > kMaxFormattedLength) {
        throw std::invalid_argument(std::format("longer than {} characters", kMaxFormattedLength));
    }
    nlohmann::json_schema::default_string_format_check(format, value);
}

}  // namespace

const std::vector<std::string>& SchemaRegistry::libraryKinds() {
    static const std::vector<std::string> kinds = {"radio-profiles", "antennas", "batteries", "drones"};
    return kinds;
}

std::string SchemaRegistry::libraryFile(const std::string& kind) {
    if (kind == "radio-profiles") return "library/radio-profile.schema.json";
    if (kind == "antennas") return "library/antenna.schema.json";
    if (kind == "batteries") return "library/battery.schema.json";
    if (kind == "drones") return "library/drone.schema.json";
    throw HttpError(404, "unknown library kind " + kind);
}

SchemaRegistry::SchemaRegistry(fs::path dir) : dir_(std::move(dir)) {
    add("mission", readJson(dir_ / "mission.schema.json"));
    for (const std::string& kind : libraryKinds()) add(kind, readJson(dir_ / libraryFile(kind)));
}

SchemaRegistry::~SchemaRegistry() = default;

void SchemaRegistry::add(const std::string& name, const json& schema) {
    const fs::path dir = dir_;
    auto loader = [dir](const nlohmann::json_uri& uri, json& out) {
        std::string location = uri.location();
        if (!location.starts_with(kBase)) throw std::invalid_argument("schema outside rfmap: " + location);
        const fs::path rel = location.substr(std::string(kBase).size());
        if (rel.empty() || rel.is_absolute() || rel.string().find("..") != std::string::npos) {
            throw std::invalid_argument("bad schema reference " + location);
        }
        out = readJson(dir / rel);
    };
    auto v = std::make_unique<nlohmann::json_schema::json_validator>(loader, boundedFormatCheck);
    v->set_root_schema(schema);
    std::lock_guard lock(mutex_);
    validators_[name] = std::move(v);
}

void SchemaRegistry::addInline(const std::string& name, const json& schema) {
    json s = schema;
    if (!s.contains("$id")) s["$id"] = std::string(kBase) + "inline/" + name + ".json";
    add(name, s);
}

std::vector<SchemaRegistry::Problem> SchemaRegistry::check(const std::string& name, const json& doc) const {
    std::lock_guard lock(mutex_);
    const auto it = validators_.find(name);
    if (it == validators_.end()) throw std::logic_error("no schema named " + name);
    Collector c;
    it->second->validate(doc, c);
    return c.problems;
}

void SchemaRegistry::require(const std::string& name, const json& doc) const {
    const auto problems = check(name, doc);
    if (problems.empty()) return;
    json list = json::array();
    for (const auto& p : problems) list.push_back({{"path", p.pointer}, {"message", p.message}});
    throw HttpError(422, "request does not match the " + name + " schema", {{"issues", list}});
}

}  // namespace rfmap
