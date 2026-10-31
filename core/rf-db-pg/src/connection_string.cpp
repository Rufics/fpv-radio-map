#include "rfdb/pg/connection_string.hpp"

#include <format>

namespace rfdb::pg {

ConnectionString::ConnectionString(std::string text)
    : text_(std::move(text)), uri_(text_.starts_with("postgresql://") || text_.starts_with("postgres://")) {}

bool ConnectionString::names(std::string_view key) const {
    // A key starts the string or follows a separator and is followed by '='.
    for (std::size_t at = text_.find(key); at != std::string::npos; at = text_.find(key, at + 1)) {
        const bool starts = at == 0 || std::string_view("?& ").find(text_[at - 1]) != std::string_view::npos;
        const std::size_t after = at + key.size();
        if (starts && after < text_.size() && text_[after] == '=') return true;
    }
    return false;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, std::string_view value) {
    if (names(key)) return *this;
    if (uri_) {
        text_ += text_.find('?') == std::string::npos ? '?' : '&';
    } else if (!text_.empty()) {
        text_ += ' ';
    }
    text_ += std::format("{}={}", key, value);
    return *this;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, long long value) {
    return withDefault(key, std::to_string(value));
}

}  // namespace rfdb::pg
