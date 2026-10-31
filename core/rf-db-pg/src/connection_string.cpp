#include "rfdb/pg/connection_string.hpp"

#include <cctype>
#include <format>

namespace rfdb::pg {

ConnectionString::ConnectionString(std::string text)
    : text_(std::move(text)), uri_(text_.starts_with("postgresql://") || text_.starts_with("postgres://")) {}

namespace {

constexpr std::string_view blanks = " \t\n\r\f\v";  // what libpq's isspace() skips

/// Reads a keyword string the way libpq does: `key = value` pairs apart by whitespace, a value
/// either single-quoted or bare, a backslash escaping the next character in both.
class KeywordReader {
public:
    explicit KeywordReader(std::string_view text) : text_(text) {}

    /// The next key, or empty at the end; its value is skipped.
    std::string_view nextKey() {
        skipBlanks();
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] != '=' && !blank(text_[at_])) ++at_;
        const std::string_view key = text_.substr(start, at_ - start);
        skipBlanks();
        if (at_ < text_.size() && text_[at_] == '=') ++at_;
        skipBlanks();
        skipValue();
        return key;
    }

private:
    static bool blank(char c) { return blanks.find(c) != std::string_view::npos; }
    void skipBlanks() {
        while (at_ < text_.size() && blank(text_[at_])) ++at_;
    }
    void skipValue() {
        const bool quoted = at_ < text_.size() && text_[at_] == '\'';
        if (quoted) ++at_;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '\\') {
                if (at_ < text_.size()) ++at_;  // a trailing backslash escapes nothing
            } else if (quoted ? c == '\'' : blank(c)) {
                return;
            }
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

/// A keyword value as libpq reads it back: quoted, with quotes and backslashes escaped, when bare
/// text would not survive.
std::string keywordValue(std::string_view value) {
    const bool bare = !value.empty() && value.find_first_of(" \t\n\r\f\v'\\") == std::string_view::npos;
    if (bare) return std::string(value);
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'' || c == '\\') quoted += '\\';
        quoted += c;
    }
    return quoted + "'";
}

/// A URI query value, percent-encoded except for the unreserved characters.
std::string uriValue(std::string_view value) {
    constexpr std::string_view unreserved = "-._~";
    std::string encoded;
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || unreserved.find(c) != std::string_view::npos) {
            encoded += c;
        } else {
            encoded += std::format("%{:02X}", byte);
        }
    }
    return encoded;
}

/// True when the text ends in a backslash that escapes nothing; libpq drops it, but a separator
/// appended after it would be escaped and glue the next setting onto that value.
bool endsInLoneBackslash(std::string_view text) {
    const std::size_t kept = text.find_last_not_of('\\');
    const std::size_t run = kept == std::string_view::npos ? text.size() : text.size() - kept - 1;
    return run % 2 == 1;
}

}  // namespace

bool ConnectionString::names(std::string_view key) const {
    if (!uri_) {
        KeywordReader reader(text_);
        for (std::string_view k = reader.nextKey(); !k.empty(); k = reader.nextKey()) {
            if (k == key) return true;
        }
        return false;
    }
    // A URI parameter starts after '?' or '&' and is followed by '='.
    for (std::size_t at = text_.find(key); at != std::string::npos; at = text_.find(key, at + 1)) {
        const std::size_t after = at + key.size();
        const bool starts = at > 0 && (text_[at - 1] == '?' || text_[at - 1] == '&');
        if (starts && after < text_.size() && text_[after] == '=') return true;
    }
    return false;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, std::string_view value) {
    if (names(key)) return *this;
    if (uri_) {
        text_ += text_.find('?') == std::string::npos ? '?' : '&';
        text_ += std::format("{}={}", key, uriValue(value));
        return *this;
    }
    if (endsInLoneBackslash(text_)) text_.pop_back();
    if (!text_.empty()) text_ += ' ';
    text_ += std::format("{}={}", key, keywordValue(value));
    return *this;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, long long value) {
    return withDefault(key, std::to_string(value));
}

}  // namespace rfdb::pg
