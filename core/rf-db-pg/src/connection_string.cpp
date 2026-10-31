#include "rfdb/pg/connection_string.hpp"

#include <algorithm>
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
        lastValueEmpty_ = at_ == text_.size();
        skipValue();
        return key;
    }

    /// Whether the last value in the text is empty, which libpq lets swallow the next setting.
    bool endsWithEmptyValue() {
        while (at_ < text_.size()) nextKey();
        return lastValueEmpty_;
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
    bool lastValueEmpty_ = false;
};

/// Writes and reads values the way libpq parses them back.
class ValueCodec {
public:
    /// A keyword value: quoted, with quotes and backslashes escaped, when bare text would not survive.
    static std::string keyword(std::string_view value) {
        if (const bool bare = !value.empty() && value.find_first_of(" \t\n\r\f\v'\\") == std::string_view::npos; bare) {
            return std::string(value);
        }
        std::string quoted = "'";
        for (const char c : value) {
            if (c == '\'' || c == '\\') quoted += '\\';
            quoted += c;
        }
        return quoted + "'";
    }

    /// A URI query value, percent-encoded except for the unreserved characters.
    static std::string uri(std::string_view value) {
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

    /// A URI query component with its %XX escapes decoded; a malformed escape stays as written.
    static std::string decoded(std::string_view text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const bool escape = text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0;
            if (escape) {
                out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
                i += 2;
            } else {
                out += text[i];
            }
        }
        return out;
    }

    /// Makes a keyword string safe to append to. A trailing backslash that escapes nothing is
    /// dropped by libpq, but a separator after it would be escaped and glue the next setting onto
    /// that value; and a value left empty would swallow the next setting, so it is quoted.
    static void closeLastValue(std::string& text) {
        const std::size_t kept = text.find_last_not_of('\\');
        const std::size_t run = kept == std::string::npos ? text.size() : text.size() - kept - 1;
        if (run % 2 == 1) text.pop_back();
        if (KeywordReader(text).endsWithEmptyValue()) text += "''";
    }

private:
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
};

}  // namespace

bool ConnectionString::names(std::string_view key) const {
    if (!uri_) {
        KeywordReader reader(text_);
        for (std::string_view k = reader.nextKey(); !k.empty(); k = reader.nextKey()) {
            if (k == key) return true;
        }
        return false;
    }
    // URI parameters follow the '?', apart by '&'; their names may be percent-encoded.
    const std::size_t query = text_.find('?');
    if (query == std::string::npos) return false;
    const std::string_view params = std::string_view(text_).substr(query + 1);
    for (std::size_t at = 0; at <= params.size();) {
        const std::size_t end = std::min(params.find('&', at), params.size());
        const std::string_view param = params.substr(at, end - at);
        if (ValueCodec::decoded(param.substr(0, param.find('='))) == key) return true;
        at = end + 1;
    }
    return false;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, std::string_view value) {
    if (names(key)) return *this;
    if (uri_) {
        text_ += text_.find('?') == std::string::npos ? '?' : '&';
        text_ += std::format("{}={}", key, ValueCodec::uri(value));
        return *this;
    }
    ValueCodec::closeLastValue(text_);
    if (!text_.empty()) text_ += ' ';
    text_ += std::format("{}={}", key, ValueCodec::keyword(value));
    return *this;
}

ConnectionString& ConnectionString::withDefault(std::string_view key, long long value) {
    return withDefault(key, std::to_string(value));
}

}  // namespace rfdb::pg
