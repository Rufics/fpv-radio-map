#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

/// Semantic version 2.0.0 (build metadata is accepted and ignored for ordering).
class Version {
public:
    static Version parse(std::string_view text);  ///< Throws std::invalid_argument.
    static std::optional<Version> tryParse(std::string_view text);

    Version() = default;
    Version(unsigned major, unsigned minor, unsigned patch) : major_(major), minor_(minor), patch_(patch) {}

    [[nodiscard]] unsigned major() const { return major_; }
    [[nodiscard]] unsigned minor() const { return minor_; }
    [[nodiscard]] unsigned patch() const { return patch_; }
    [[nodiscard]] const std::vector<std::string>& prerelease() const { return pre_; }
    [[nodiscard]] bool isPrerelease() const { return !pre_.empty(); }
    [[nodiscard]] std::string str() const;

    friend std::strong_ordering operator<=>(const Version& a, const Version& b) { return compare(a, b); }
    friend bool operator==(const Version& a, const Version& b) { return (a <=> b) == 0; }

private:
    /// SemVer precedence (build metadata is not stored, so it never counts).
    static std::strong_ordering compare(const Version& a, const Version& b);

    unsigned major_ = 0;
    unsigned minor_ = 0;
    unsigned patch_ = 0;
    std::vector<std::string> pre_;
};

/// A version range: comparators joined by spaces (AND) and "||" (OR).
/// Supported: =, >, >=, <, <=, ^, ~, and a bare version (meaning =). "*" or "" matches all.
/// As in npm, a prerelease version only matches a comparator set that names a prerelease
/// of the same major.minor.patch.
class VersionRange {
public:
    static VersionRange parse(std::string_view text);  ///< Throws std::invalid_argument.

    [[nodiscard]] bool contains(const Version& v) const;
    [[nodiscard]] const std::string& str() const { return text_; }

private:
    enum class Op { Eq, Gt, Ge, Lt, Le };
    struct Comparator {
        Op op{};
        Version version;
    };
    using Set = std::vector<Comparator>;

    static void appendComparators(std::string_view token, Set& out);
    /// One "||" alternative: space-separated comparators, all of which must match.
    static Set parseSet(std::string_view alternative);

    std::string text_;
    std::vector<Set> sets_;
};

}  // namespace rfmod
