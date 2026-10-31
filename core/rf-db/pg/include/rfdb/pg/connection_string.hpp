#pragma once

#include <string>
#include <string_view>

namespace rfdb::pg {

/// A libpq connection string, as a URI ("postgresql://...") or as keywords ("host=... dbname=...").
/// Adds client-side parameters the operator did not set, so their own settings always win.
class ConnectionString {
public:
    explicit ConnectionString(std::string text);

    /// Adds `key=value` unless the string names `key` already.
    ConnectionString& withDefault(std::string_view key, std::string_view value);
    ConnectionString& withDefault(std::string_view key, long long value);

    [[nodiscard]] const std::string& text() const { return text_; }

private:
    [[nodiscard]] bool names(std::string_view key) const;

    std::string text_;
    bool uri_;
};

}  // namespace rfdb::pg
