#pragma once

#include "common.hpp"
#include "database.hpp"
#include "schemas.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rfmap {

/// Which version of which library item a result used.
struct LibraryRef {
    std::string kind;
    std::string id;
    int version;
    bool verified;
    [[nodiscard]] json toJson() const { return {{"kind", kind}, {"id", id}, {"version", version}, {"verified", verified}}; }
};

/// The reference library (radio profiles, antennas, batteries, drones). Every number in it comes
/// from a datasheet or measurement with its source; nothing of it lives in code. Only the admin
/// writes; each save bumps the version and keeps the old one in library_history.
class Library {
public:
    Library(Database& db, const SchemaRegistry& schemas) : db_(db), schemas_(schemas) {}

    [[nodiscard]] std::vector<json> list(const std::string& kind) const;
    [[nodiscard]] std::optional<json> get(const std::string& kind, const std::string& id) const;
    [[nodiscard]] std::vector<json> history(const std::string& kind, const std::string& id) const;

    /// New item at version 1. 409 if the id exists, 422 with the schema report if invalid.
    json add(const std::string& kind, json doc, const std::string& by);
    /// Saves a new version. The body's `version` must be the one being edited (409 otherwise);
    /// the stored item gets version + 1.
    json update(const std::string& kind, const std::string& id, json doc, const std::string& by);

    /// Imports each <dir>/<kind>/seed*.json once, tracked by file name, in name order. Seed
    /// items are always stored unverified, whatever the file says. Returns lines for the
    /// start-up log.
    std::vector<std::string> seed(const std::filesystem::path& dir);

    /// Item or HttpError 422 naming what is missing (used when a mission references it).
    [[nodiscard]] std::pair<json, LibraryRef> require(const std::string& kind, const std::string& id) const;

private:
    /// The kind's seed files (seed*.json), sorted by name.
    [[nodiscard]] static std::vector<std::filesystem::path> seedFiles(const std::filesystem::path& kindDir);
    /// Imports one seed file unless it was imported before, appending to the start-up log.
    void seedFile(const std::string& kind, const std::filesystem::path& file, std::vector<std::string>& log);

    Database& db_;
    const SchemaRegistry& schemas_;
};

}  // namespace rfmap
