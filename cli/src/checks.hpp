#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace rfmap::cli {

using json = nlohmann::json;

/// What a CI run asks of a calculation; a result that falls short makes the tool exit with 1.
struct Requirements {
    /// The least severe leg status that fails the run: 1 = warn, 2 = blocked (missing-data counts
    /// as blocked: nothing confirms the link there). Empty = statuses do not fail.
    std::optional<int> failOnSeverity;
    /// The least margin every evaluated link must keep, dB (marginDb of the worst sample).
    std::optional<double> minMarginDb;

    /// "warn" → 1, "blocked" → 2; anything else is a UsageError.
    static int severityOf(const std::string& failOn);
    [[nodiscard]] bool any() const { return failOnSeverity.has_value() || minMarginDb.has_value(); }
};

/// 0 ok, 1 warn, 2 blocked or missing-data, for a leg status.
int legSeverity(const std::string& status);

/// The ways a POST /calc/route result falls short, one line each; empty when it meets them.
std::vector<std::string> routeShortfalls(const json& route, const Requirements& required);
/// The same for POST /calc/path: one LinkResult, or {control?, video?}.
std::vector<std::string> pathShortfalls(const json& path, const Requirements& required);

/// One line per leg: status, length and each link's margin, for a person reading the CI log.
std::string routeSummary(const json& route);
/// One line per evaluated link of a path result.
std::string pathSummary(const json& path);

}  // namespace rfmap::cli
