#pragma once

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap::cli {

/// A command line the tool cannot run: exit status 64 and the usage text.
class UsageError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/// The words of a command line: positional words in order, `--flag VALUE` (or `--flag=VALUE`)
/// pairs, and the few flags that take no value. Flags may come before or after the command.
class Arguments {
public:
    /// Flags that stand alone; every other flag takes the next word (or its `=` part) as value.
    static const std::set<std::string, std::less<>>& switches();
    /// Flags every command takes: the server, the credentials and how results are written.
    static const std::set<std::string, std::less<>>& globalFlags();

    explicit Arguments(std::span<const std::string> words);

    [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }
    /// The positional word at `index`, or UsageError naming `what`.
    [[nodiscard]] const std::string& word(std::size_t index, const char* what) const;
    [[nodiscard]] bool has(std::string_view flag) const { return flags_.contains(flag); }
    /// The flag's last value, if it was given.
    [[nodiscard]] std::optional<std::string> get(std::string_view flag) const;
    /// The flag's value; UsageError when it is missing or empty.
    [[nodiscard]] std::string need(std::string_view flag) const;
    /// The flag's value as a finite number; UsageError when it is not one.
    [[nodiscard]] std::optional<double> number(std::string_view flag) const;

    /// UsageError naming the first flag that is neither global nor in `allowed`, and when there
    /// are more than `maxPositional` positional words: a typo fails instead of being ignored.
    void requireOnly(std::initializer_list<std::string_view> allowed, std::size_t maxPositional) const;

    /// "lat,lon" (`withHeight` false) or "lat,lon,agl" as numbers; UsageError naming `flag`.
    static std::vector<double> coordinates(std::string_view text, std::string_view flag, bool withHeight);
    static double parseNumber(std::string_view text, std::string_view flag);

private:
    std::vector<std::string> positional_;
    std::map<std::string, std::vector<std::string>, std::less<>> flags_;
};

}  // namespace rfmap::cli
