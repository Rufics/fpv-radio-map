#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rfmod {

/// A JSON text nested deeper than BoundedJson allows.
class JsonTooDeepError : public std::runtime_error {
public:
    explicit JsonTooDeepError(std::size_t limit);
};

/// Parses JSON from a source that is not trusted: a request body, a worker's reply.
///
/// nlohmann::json parses without recursion, but copying, comparing, dumping and validating a
/// value recurse once per level, so `[[[[...]]]]` a megabyte deep overflows the stack of the
/// thread that touches it. The nesting is counted on the text first, in one pass that allocates
/// nothing, and a text deeper than the limit is refused before a value is built.
class BoundedJson {
public:
    /// Far beyond any document the API or a module exchanges, far below what overflows a stack.
    static constexpr std::size_t kMaxDepth = 64;

    /// The parsed value. JsonTooDeepError when the text nests deeper than `maxDepth`, and
    /// nlohmann::json::parse_error when it is not JSON.
    [[nodiscard]] static nlohmann::json parse(std::string_view text, std::size_t maxDepth = kMaxDepth);

    /// Whether the arrays and objects in `text` nest at most `maxDepth` deep. Brackets inside
    /// strings do not count; whether the text is valid JSON is the parser's to decide.
    [[nodiscard]] static bool withinDepth(std::string_view text, std::size_t maxDepth);
};

}  // namespace rfmod
