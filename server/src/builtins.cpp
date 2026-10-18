#include "builtins.hpp"

namespace rfmap {

BuiltinModules::BuiltinModules(Database* db, SchemaRegistry& schemas, std::string version)
    : db_(db), schemas_(schemas), version_(std::move(version)) {
    const json noOptions = {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()}};
    // Clutter heights are the admin's figures with their source: a class code alone does not
    // say how tall the trees or buildings are, so rfmap ships no number.
    const json clutterOptions = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"classes",
           {{"type", "object"},
            {"propertyNames", {{"enum", {"10", "20", "30", "40", "50", "60", "70", "80", "90", "95", "100"}}}},
            {"additionalProperties",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"enabled"}},
              {"properties",
               {{"enabled", {{"type", "boolean"}}},
                {"heightM", {{"type", "number"}, {"minimum", 0}, {"maximum", 500}}},
                {"source", {{"type", "string"}, {"maxLength", 300}}}}}}}}}}}};
    entries_[kWeather] = {"Weather and refraction",
                          "Forecast refractivity gradient gives k per path (ITU-R P.453-14); forecast rain adds a "
                          "loss term (ITU-R P.838-3). Opt in per mission with modules[\"rf.weather\"].",
                          {"loss.contributor", "refraction"}, noOptions, true, json::object()};
    entries_[kClutter] = {"Land cover clutter",
                          "Raises the terrain profile by the admin's height per ESA WorldCover class; treated as opaque. "
                          "Opt in per mission with modules[\"rf.clutter\"], {\"disabledClasses\": [codes]} turns classes off.",
                          {"profile.conditioner"}, clutterOptions, true, json::object()};
    entries_[kUncertainty] = {"Uncertainty band",
                              "Margin band and verdict from the DEM's stated vertical error, propagated exactly "
                              "through geometry and diffraction.",
                              {"result.band"}, noOptions, true, json::object()};
    entries_[kRadioProfiles] = {"Radio profiles",
                                "The radio-profiles library kind, seeded unverified from data/library/radio-profiles.",
                                {"library.kind"}, noOptions, true, json::object()};
    for (auto& [id, e] : entries_) schemas_.addInline("builtin:" + id, e.optionsSchema);

    if (!db_) return;
    db_->tx([&](pqxx::work& w) {
        for (auto& [id, e] : entries_) {
            w.exec_params("insert into builtin_modules(id) values ($1) on conflict do nothing", id);
            const auto r = w.exec_params("select enabled, options::text from builtin_modules where id = $1", id);
            e.enabled = r[0][0].as<bool>();
            e.options = json::parse(r[0][1].as<std::string>());
        }
    });
}

bool BuiltinModules::isBuiltin(const std::string& id) const { return entries_.count(id) > 0; }

bool BuiltinModules::enabled(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    return it != entries_.end() && it->second.enabled;
}

json BuiltinModules::options(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(id);
    return it == entries_.end() ? json::object() : it->second.options;
}

void BuiltinModules::set(const std::string& id, std::optional<bool> enabled, std::optional<json> options) {
    if (!isBuiltin(id)) throw HttpError(404, "no built-in module " + id);
    if (options) schemas_.require("builtin:" + id, *options);
    std::lock_guard lock(mutex_);
    Entry& e = entries_.at(id);
    if (enabled.has_value()) e.enabled = *enabled;
    if (options) e.options = *options;
    if (db_) {
        db_->tx([&](pqxx::work& w) {
            w.exec_params("update builtin_modules set enabled = $2, options = $3::jsonb, updated_at = now() where id = $1", id,
                          e.enabled, e.options.dump());
        });
    }
}

json BuiltinModules::list() const {
    std::lock_guard lock(mutex_);
    json out = json::array();
    for (const auto& [id, e] : entries_) {
        out.push_back({{"id", id},
                       {"version", version_},
                       {"title", e.title},
                       {"description", e.description},
                       {"lifecycle", "stable"},
                       {"requires", json::array()},
                       {"optional", json::array()},
                       {"provides", e.provides},
                       {"signedBy", "built in"},
                       {"builtIn", true},
                       {"isolated", false},
                       {"options", e.optionsSchema},
                       {"settings", e.options},
                       {"state", e.enabled ? "active" : "disabled"}});
    }
    return out;
}

}  // namespace rfmap
