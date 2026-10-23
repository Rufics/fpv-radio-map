#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace rfmod {

/// A module's options schema, compiled once per load so a mission's per-module options can be
/// checked on every request without touching the disk.
class OptionsSchema {
public:
    /// Throws std::invalid_argument when the schema document cannot be used.
    explicit OptionsSchema(const nlohmann::json& schema);
    ~OptionsSchema();
    OptionsSchema(const OptionsSchema&) = delete;
    OptionsSchema& operator=(const OptionsSchema&) = delete;

    [[nodiscard]] const nlohmann::json& document() const { return document_; }
    /// The reason the options are rejected, or nullopt when they match.
    [[nodiscard]] std::optional<std::string> check(const nlohmann::json& options) const;

private:
    class Validator;
    nlohmann::json document_;
    std::unique_ptr<Validator> validator_;
};

}  // namespace rfmod
