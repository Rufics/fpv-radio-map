#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace rfmod {

/// The install validation report (rfmod check, rfmod install): every check is listed, passed or not,
/// so the admin sees exactly why a package was refused.
class ValidationReport {
public:
    struct Check {
        std::string name;
        bool ok;
        std::string detail;
    };

    void pass(std::string name, std::string detail = {}) { add(std::move(name), true, std::move(detail)); }
    void fail(std::string name, std::string detail) { add(std::move(name), false, std::move(detail)); }
    void add(std::string name, bool ok, std::string detail) {
        checks_.emplace_back(std::move(name), ok, std::move(detail));
    }
    void merge(const ValidationReport& other) { checks_.insert(checks_.end(), other.checks_.begin(), other.checks_.end()); }

    [[nodiscard]] bool ok() const { return std::ranges::all_of(checks_, &Check::ok); }
    [[nodiscard]] const std::vector<Check>& checks() const { return checks_; }

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json list = nlohmann::json::array();
        for (const Check& c : checks_) list.push_back({{"name", c.name}, {"ok", c.ok}, {"detail", c.detail}});
        return {{"ok", ok()}, {"checks", list}};
    }

private:
    std::vector<Check> checks_;
};

}  // namespace rfmod
