#include "rfmod/options_schema.hpp"

#include <nlohmann/json-schema.hpp>

#include <stdexcept>

namespace rfmod {

class OptionsSchema::Validator {
public:
    explicit Validator(const nlohmann::json& schema) { validator_.set_root_schema(schema); }

    [[nodiscard]] std::optional<std::string> check(const nlohmann::json& options) const {
        try {
            validator_.validate(options);
            return std::nullopt;
        } catch (const std::invalid_argument& e) {  // the validator's verdict
            return std::string("options rejected: ") + e.what();
        }
    }

private:
    nlohmann::json_schema::json_validator validator_;
};

OptionsSchema::OptionsSchema(const nlohmann::json& schema)
    : document_(schema), validator_(std::make_unique<Validator>(schema)) {}

OptionsSchema::~OptionsSchema() = default;

std::optional<std::string> OptionsSchema::check(const nlohmann::json& options) const { return validator_->check(options); }

}  // namespace rfmod
