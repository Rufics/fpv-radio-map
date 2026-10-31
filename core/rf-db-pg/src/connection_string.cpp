#include "rfdb/pg/connection_string.hpp"

#include <format>

namespace rfdb::pg {

ConnectionString::ConnectionString(std::string text)
    : text_(std::move(text)), uri_(text_.starts_with("postgresql://") || text_.starts_with("postgres://")) {}

bool ConnectionString::names(std::string_view key) const {
    // A key starts the string or follows a separator and is followed by '='; the keyword form
    // allows whitespace around the '=' (libpq skips what isspace() accepts).
    constexpr std::string_view blanks = " \t\n\r\f\v";
    const std::string_view separators = uri_ ? "?&" : blanks;
    for (std::size_t at = text_.find(key); at != std::string::npos; at = text_.find(key, at + 1)) {
        const bool starts = at == 0 || separators.find(text_[at - 1]) != std::string_view::npos;
        std::size_t after = at + key.size();
        while (!uri_ && after < text_.size() && blanks.find(text_[after]) != std::string_view::npos) ++after;
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
