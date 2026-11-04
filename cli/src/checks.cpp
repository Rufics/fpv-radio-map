#include "checks.hpp"

#include "arguments.hpp"

#include <format>

namespace rfmap::cli {
namespace {

/// The links of a path result: a single LinkResult names its kind, a both-links result is keyed.
std::vector<std::pair<std::string, json>> pathLinks(const json& path) {
    std::vector<std::pair<std::string, json>> out;
    if (!path.is_object()) return out;
    if (path.contains("kind")) {
        out.emplace_back(path.value("kind", "link"), path);
        return out;
    }
    for (const char* kind : {"control", "video"}) {
        if (path.contains(kind) && path.at(kind).is_object()) out.emplace_back(kind, path.at(kind));
    }
    return out;
}

std::optional<double> margin(const json& link) {
    if (link.is_object() && link.contains("marginDb") && link.at("marginDb").is_number()) {
        return link.at("marginDb").get<double>();
    }
    return std::nullopt;
}

std::string legName(const json& leg) {
    return std::format("leg {} ({} -> {})", leg.value("index", 0) + 1, leg.value("from", "?"), leg.value("to", "?"));
}

std::string margins(const json& worst) {
    std::string out;
    for (const char* kind : {"control", "video"}) {
        if (!worst.is_object() || !worst.contains(kind)) continue;
        if (const auto m = margin(worst.at(kind))) out += std::format("  {} {:+.1f} dB", kind, *m);
    }
    return out;
}

}  // namespace

int Requirements::severityOf(const std::string& failOn) {
    if (failOn == "warn") return 1;
    if (failOn == "blocked") return 2;
    throw UsageError("--fail-on takes warn or blocked, not \"" + failOn + "\"");
}

int legSeverity(const std::string& status) {
    if (status == "ok") return 0;
    if (status == "warn") return 1;
    return 2;  // blocked, missing-data, or a status this client does not know yet
}

std::vector<std::string> routeShortfalls(const json& route, const Requirements& required) {
    std::vector<std::string> out;
    if (!route.is_object() || !route.contains("legs") || !route.at("legs").is_array()) {
        if (required.any()) out.emplace_back("the result has no legs to check");
        return out;
    }
    for (const json& leg : route.at("legs")) {
        const std::string status = leg.value("status", "unknown");
        if (required.failOnSeverity && legSeverity(status) >= *required.failOnSeverity) {
            out.push_back(std::format("{} is {}", legName(leg), status));
        }
        if (!required.minMarginDb || !leg.contains("worst") || !leg.at("worst").is_object()) continue;
        for (const auto& [kind, link] : leg.at("worst").items()) {
            const auto m = margin(link);
            if (m && *m < *required.minMarginDb) {
                out.push_back(std::format("{}: {} margin {:+.1f} dB is below {:.1f} dB", legName(leg), kind, *m,
                                          *required.minMarginDb));
            }
        }
    }
    return out;
}

std::vector<std::string> pathShortfalls(const json& path, const Requirements& required) {
    std::vector<std::string> out;
    const auto links = pathLinks(path);
    if (links.empty() && required.any()) out.emplace_back("the result has no link to check");
    for (const auto& [kind, link] : links) {
        const auto m = margin(link);
        if (required.minMarginDb && m && *m < *required.minMarginDb) {
            out.push_back(std::format("{} margin {:+.1f} dB is below {:.1f} dB", kind, *m, *required.minMarginDb));
        }
        // A path has no leg status: an unreachable link is what blocked means for it.
        if (required.failOnSeverity && link.contains("reachable") && !link.value("reachable", true)) {
            out.push_back(kind + " link is not reachable");
        }
    }
    return out;
}

std::string routeSummary(const json& route) {
    std::string out;
    if (!route.is_object() || !route.contains("legs") || !route.at("legs").is_array()) return out;
    for (const json& leg : route.at("legs")) {
        out += std::format("{:<32} {:>8.2f} km  {:<12}{}\n", legName(leg), leg.value("lengthM", 0.0) / 1000.0,
                           leg.value("status", "?"), margins(leg.value("worst", json::object())));
    }
    if (route.contains("moduleFailures") && route.at("moduleFailures").is_array()) {
        for (const json& f : route.at("moduleFailures")) {
            out += std::format("module {} failed: {}\n", f.value("module", "?"), f.value("error", f.dump()));
        }
    }
    return out;
}

std::string pathSummary(const json& path) {
    std::string out;
    for (const auto& [kind, link] : pathLinks(path)) {
        const auto m = margin(link);
        out += std::format("{:<8} {:>8.2f} km  {}  margin {}\n", kind, link.value("distanceM", 0.0) / 1000.0,
                           link.value("reachable", false) ? "reachable  " : "unreachable",
                           m ? std::format("{:+.1f} dB", *m) : std::string("?"));
    }
    return out;
}

}  // namespace rfmap::cli
