#pragma once

#include "common.hpp"
#include "database.hpp"
#include "security.hpp"

#include <optional>
#include <string>

namespace rfmap {

struct MissionRecord {
    std::string id;
    json doc;
    int revision;
    std::optional<json> lastResult;
};

/// Missions in PostgreSQL. The pilot position is kept as PostGIS geography too, for the
/// admin's "where is the map used" queries and future spatial lookups.
class MissionStore {
public:
    explicit MissionStore(Database& db) : db_(db) {}

    std::string create(const json& doc, const Digest& ownerKeyHash);
    [[nodiscard]] std::optional<MissionRecord> get(const std::string& id) const;
    /// Optimistic locking: saves only if the stored revision is `expected`. Returns the new
    /// revision, or nothing when someone saved in between.
    std::optional<int> save(const std::string& id, const json& doc, int expected);
    /// Stores a route result as the mission's last result if it was computed for the saved doc.
    void storeResultIfCurrent(const std::string& id, const json& doc, const json& result);

private:
    Database& db_;
};

}  // namespace rfmap
