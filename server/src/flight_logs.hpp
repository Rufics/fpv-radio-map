#pragma once

#include "builtins.hpp"
#include "common.hpp"
#include "database.hpp"
#include "missions.hpp"
#include "module_runner.hpp"
#include "schemas.hpp"

#include <rfflightlog/processor.hpp>

#include <optional>
#include <string>
#include <vector>

namespace rfmap {

/// Flight logs imported into missions, one row per flight, in PostgreSQL.
class FlightLogStore {
public:
    explicit FlightLogStore(Database& db) : db_(db) {}

    struct NewFlight {
        int index = 0;
        std::optional<std::string> startedAt;
        double durationS = 0.0;
        json normalized;  ///< a flightlog/v1 document holding this one flight
        json summary;
    };
    struct Stored {
        std::string id;
        std::string fileName;
        std::string decoder;
        json normalized;
    };

    /// Adds the flights of one file. A flight imported before (same file bytes and index) is
    /// kept, and its id returned again. Stores nothing and answers nullopt when the flights new
    /// to the mission would take it past maxPerMission.
    std::optional<std::vector<std::string>> add(const std::string& missionId, const std::string& fileName,
                                                const std::string& sha256, const std::string& decoder,
                                                const std::vector<NewFlight>& flights, int maxPerMission);
    [[nodiscard]] json list(const std::string& missionId) const;  ///< logbook rows, newest first
    [[nodiscard]] std::optional<Stored> get(const std::string& missionId, const std::string& id) const;
    bool remove(const std::string& missionId, const std::string& id);

private:
    Database& db_;
};

/// Flight log replay: hands an uploaded file to the installed decoder modules (they provide
/// flightlog.decoder and always run isolated), checks their flightlog/v1 answer, and turns it
/// into the replay view with rf-flightlog. The same path serves every log format.
class FlightLogService {
public:
    struct Limits {
        std::size_t maxBytes = 64u << 20;
        int maxPerMission = 200;
    };

    FlightLogService(ModuleRunner& modules, const SchemaRegistry& schemas, FlightLogStore* store,
                     const BuiltinModules& builtins, Limits limits);

    /// Decodes and processes without storing: the replay view of every flight in the file.
    [[nodiscard]] json preview(const std::string& bytes, const std::string& fileName) const;
    /// Decodes, processes and stores every flight of the file in the mission.
    [[nodiscard]] json import(const std::string& missionId, const std::string& bytes, const std::string& fileName);
    [[nodiscard]] json list(const std::string& missionId) const;
    /// One stored flight's replay view, with plan against fact for the mission's route.
    [[nodiscard]] json view(const MissionRecord& mission, const std::string& id) const;
    void remove(const std::string& missionId, const std::string& id);

    /// The file name as it is kept and shown: no directories, no control characters, bounded.
    [[nodiscard]] static std::string cleanFileName(const std::string& name);

private:
    struct Decoded {
        std::string decoder;
        json document;
    };
    [[nodiscard]] Decoded decode(const std::string& bytes, const std::string& fileName) const;
    [[nodiscard]] static bool probe(rfmod::IModuleInstance& decoder, const json& request);
    [[nodiscard]] static json invoke(rfmod::IModuleInstance& decoder, const std::string& method, const json& request);
    [[nodiscard]] rfflightlog::ProcessOptions options() const;
    void requireEnabled() const;
    void requireStore() const;
    [[nodiscard]] static json single(const json& document, const json& entry);

    ModuleRunner& modules_;
    const SchemaRegistry& schemas_;
    FlightLogStore* store_;
    const BuiltinModules& builtins_;
    Limits limits_;
    rfflightlog::FlightLogProcessor processor_;
};

}  // namespace rfmap
