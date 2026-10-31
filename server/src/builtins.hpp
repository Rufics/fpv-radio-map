#pragma once

#include "common.hpp"
#include "database.hpp"
#include "schemas.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rfmap {

/// The RF extensions compiled into the server (rf-ext), listed in GET /modules next to the
/// installed rfmod packages. The admin can switch each off and set its options; the state is
/// kept in the builtin_modules table.
///
///  rf.weather        forecast refraction (k per path, ITU-R P.453) and rain (ITU-R P.838-3)
///  rf.clutter        land cover obstacles (ESA WorldCover classes, heights set by the admin)
///  rf.uncertainty    margin band from the DEM's stated error
///  rf.radio-profiles the radio profile library kind and its seed
///  rf.flightlog      flight log replay (rf-flightlog): timeline rate and story thresholds
class BuiltinModules {
public:
    static constexpr const char* kWeather = "rf.weather";
    static constexpr const char* kClutter = "rf.clutter";
    static constexpr const char* kUncertainty = "rf.uncertainty";
    static constexpr const char* kRadioProfiles = "rf.radio-profiles";
    static constexpr const char* kFlightLog = "rf.flightlog";

    BuiltinModules(Database* db, SchemaRegistry& schemas, std::string version);

    [[nodiscard]] bool enabled(const std::string& id) const;
    [[nodiscard]] json options(const std::string& id) const;
    [[nodiscard]] bool isBuiltin(const std::string& id) const;
    /// Validates options against the entry's schema (422 otherwise).
    void set(const std::string& id, std::optional<bool> enabled, std::optional<json> options);
    [[nodiscard]] json list() const;  ///< ModuleInfo[] entries
    /// Reads the switches and options from the table again: another server process, or the
    /// admin by hand, may have changed them (SettingsSync).
    void reload();

private:
    struct Entry {
        std::string title;
        std::string description;
        std::vector<std::string> provides;
        json optionsSchema;
        bool enabled = true;
        json options = json::object();
    };
    Database* db_;
    SchemaRegistry& schemas_;
    std::string version_;
    std::map<std::string, Entry, std::less<>> entries_;
    mutable std::mutex mutex_;
};

}  // namespace rfmap
