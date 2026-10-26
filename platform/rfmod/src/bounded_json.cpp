#include "rfmod/bounded_json.hpp"

#include <format>

namespace rfmod {

namespace {

/// Walks a JSON text and tracks how deep its arrays and objects are open.
class NestingScanner {
public:
    /// Takes one character; false once the nesting exceeds `limit`.
    bool step(char c, std::size_t limit) {
        if (inString_) {
            readString(c);
            return true;
        }
        if (c == '"') {
            inString_ = true;
        } else if (c == '[' || c == '{') {
            ++depth_;
        } else if ((c == ']' || c == '}') && depth_ > 0) {
            --depth_;
        }
        return depth_ <= limit;
    }

private:
    void readString(char c) {
        if (escaped_) {
            escaped_ = false;
        } else if (c == '\\') {
            escaped_ = true;
        } else if (c == '"') {
            inString_ = false;
        }
    }

    std::size_t depth_ = 0;
    bool inString_ = false;
    bool escaped_ = false;
};

}  // namespace

JsonTooDeepError::JsonTooDeepError(std::size_t limit)
    : std::runtime_error(std::format("JSON nests deeper than {} levels", limit)) {}

bool BoundedJson::withinDepth(std::string_view text, std::size_t maxDepth) {
    NestingScanner scanner;
    for (const char c : text) {
        if (!scanner.step(c, maxDepth)) return false;
    }
    return true;
}

nlohmann::json BoundedJson::parse(std::string_view text, std::size_t maxDepth) {
    if (!withinDepth(text, maxDepth)) throw JsonTooDeepError(maxDepth);
    return nlohmann::json::parse(text);
}

}  // namespace rfmod
