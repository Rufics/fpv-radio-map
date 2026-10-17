#pragma once

#include "common.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nlohmann::json_schema {
class json_validator;
}

namespace rfmap {

/// The draft-07 schemas under schemas/, loaded once. References between files
/// ("common.schema.json#/$defs/id", "mission.schema.json#/$defs/point") resolve against the
/// files' $id (https://rfmap.local/schemas/...), mapped onto the schemas directory.
class SchemaRegistry {
public:
    explicit SchemaRegistry(std::filesystem::path dir);
    ~SchemaRegistry();

    struct Problem {
        std::string pointer;
        std::string message;
    };

    /// All problems found; empty when valid. `name` is "mission", a library kind such as
    /// "radio-profiles", or a request schema registered with addInline.
    [[nodiscard]] std::vector<Problem> check(const std::string& name, const json& doc) const;
    /// Throws HttpError 422 with {"issues": [{path, message}]} (path is a JSON pointer).
    void require(const std::string& name, const json& doc) const;

    /// Registers a schema written in code for a request body; it may $ref the files.
    void addInline(const std::string& name, const json& schema);

    /// Library kind (URL form, "radio-profiles") to its schema file.
    static std::string libraryFile(const std::string& kind);
    static const std::vector<std::string>& libraryKinds();

private:
    void add(const std::string& name, const json& schema);
    std::filesystem::path dir_;
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>> validators_;
    mutable std::mutex mutex_;
};

}  // namespace rfmap
