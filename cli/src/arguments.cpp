#include "arguments.hpp"

#include <charconv>
#include <cmath>
#include <ranges>

namespace rfmap::cli {

const std::set<std::string, std::less<>>& Arguments::switches() {
    static const std::set<std::string, std::less<>> s{"--summary", "--compact", "--help", "--version", "--no-wait"};
    return s;
}

const std::set<std::string, std::less<>>& Arguments::globalFlags() {
    static const std::set<std::string, std::less<>> g{"--server", "--token",  "--token-file", "--timeout",
                                                      "--retries", "--output", "--compact",    "--help"};
    return g;
}

Arguments::Arguments(std::span<const std::string> words) {
    auto next = words.begin();
    while (next != words.end()) {
        std::string word = *next++;
        if (word == "-h") word = "--help";
        if (!word.starts_with("--") || word == "--") {
            positional_.push_back(word);
            continue;
        }
        if (const auto eq = word.find('='); eq != std::string::npos) {
            flags_[word.substr(0, eq)].push_back(word.substr(eq + 1));
            continue;
        }
        if (switches().contains(word)) {
            flags_[word].emplace_back();
            continue;
        }
        if (next == words.end()) throw UsageError("missing value for " + word);
        flags_[word].push_back(*next++);
    }
}

const std::string& Arguments::word(std::size_t index, const char* what) const {
    if (index >= positional_.size()) throw UsageError(std::string("missing ") + what);
    return positional_[index];
}

std::optional<std::string> Arguments::get(std::string_view flag) const {
    const auto it = flags_.find(flag);
    if (it == flags_.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

std::string Arguments::need(std::string_view flag) const {
    const auto v = get(flag);
    if (!v || v->empty()) throw UsageError(std::string(flag) + " is required");
    return *v;
}

std::optional<double> Arguments::number(std::string_view flag) const {
    const auto v = get(flag);
    if (!v) return std::nullopt;
    return parseNumber(*v, flag);
}

void Arguments::requireOnly(std::initializer_list<std::string_view> allowed, std::size_t maxPositional) const {
    for (const std::string& flag : flags_ | std::views::keys) {
        if (globalFlags().contains(flag) || std::ranges::find(allowed, flag) != allowed.end()) continue;
        throw UsageError("unknown option " + flag + " for this command");
    }
    if (positional_.size() > maxPositional) throw UsageError("unexpected argument " + positional_[maxPositional]);
}

double Arguments::parseNumber(std::string_view text, std::string_view flag) {
    double value = 0.0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || !std::isfinite(value)) {
        throw UsageError(std::string(flag) + " needs a number, not \"" + std::string(text) + "\"");
    }
    return value;
}

std::vector<double> Arguments::coordinates(std::string_view text, std::string_view flag, bool withHeight) {
    std::vector<double> out;
    for (const auto part : text | std::views::split(',')) {
        out.push_back(parseNumber(std::string_view(part.begin(), part.end()), flag));
    }
    if (const std::size_t want = withHeight ? 3 : 2; out.size() != want) {
        throw UsageError(std::string(flag) + (withHeight ? " needs lat,lon,agl" : " needs lat,lon") + ", not \"" +
                         std::string(text) + "\"");
    }
    if (out[0] < -90.0 || out[0] > 90.0 || out[1] < -180.0 || out[1] > 180.0) {
        throw UsageError(std::string(flag) + " is outside the world: " + std::string(text));
    }
    return out;
}

}  // namespace rfmap::cli
