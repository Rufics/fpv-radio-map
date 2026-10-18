#include "rfmod/semver.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace rfmod {
namespace {

bool isDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool isIdentChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

unsigned parseNumber(std::string_view s) {
    if (!isDigits(s) || (s.size() > 1 && s.front() == '0')) {
        throw std::invalid_argument("invalid version number: " + std::string(s));
    }
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size()) {
        throw std::invalid_argument("version number out of range: " + std::string(s));
    }
    return value;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

/// Throws unless `id` is a non-empty run of [0-9A-Za-z-]; `kind` and `text` go into the message.
void checkIdentifier(std::string_view id, std::string_view kind, std::string_view text) {
    if (id.empty()) throw std::invalid_argument("empty " + std::string(kind) + " identifier in " + std::string(text));
    if (!std::ranges::all_of(id, isIdentChar)) {
        throw std::invalid_argument("bad " + std::string(kind) + " identifier in " + std::string(text));
    }
}

}  // namespace

Version Version::parse(std::string_view text) {
    std::string_view s = text;
    if (const auto plus = s.find('+'); plus != std::string_view::npos) {
        for (const std::string_view id : split(s.substr(plus + 1), '.')) checkIdentifier(id, "build", text);
        s = s.substr(0, plus);
    }
    Version v;
    if (const auto dash = s.find('-'); dash != std::string_view::npos) {
        for (std::string_view id : split(s.substr(dash + 1), '.')) {
            checkIdentifier(id, "prerelease", text);
            if (isDigits(id) && id.size() > 1 && id.front() == '0') {
                throw std::invalid_argument("leading zero in prerelease of " + std::string(text));
            }
            v.pre_.emplace_back(id);
        }
        s = s.substr(0, dash);
    }
    const auto parts = split(s, '.');
    if (parts.size() != 3) throw std::invalid_argument("version must be MAJOR.MINOR.PATCH: " + std::string(text));
    v.major_ = parseNumber(parts[0]);
    v.minor_ = parseNumber(parts[1]);
    v.patch_ = parseNumber(parts[2]);
    return v;
}

std::optional<Version> Version::tryParse(std::string_view text) {
    try {
        return parse(text);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

std::string Version::str() const {
    std::string out = std::to_string(major_) + "." + std::to_string(minor_) + "." + std::to_string(patch_);
    for (std::size_t i = 0; i < pre_.size(); ++i) {
        out += (i == 0 ? "-" : ".") + pre_[i];
    }
    return out;
}

std::strong_ordering operator<=>(const Version& a, const Version& b) {
    if (auto c = a.major_ <=> b.major_; c != 0) return c;
    if (auto c = a.minor_ <=> b.minor_; c != 0) return c;
    if (auto c = a.patch_ <=> b.patch_; c != 0) return c;
    // A release ranks above any of its prereleases (SemVer §11.3).
    if (a.pre_.empty() && b.pre_.empty()) return std::strong_ordering::equal;
    if (a.pre_.empty()) return std::strong_ordering::greater;
    if (b.pre_.empty()) return std::strong_ordering::less;
    const std::size_t n = std::min(a.pre_.size(), b.pre_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& x = a.pre_[i];
        const std::string& y = b.pre_[i];
        const bool xn = isDigits(x);
        const bool yn = isDigits(y);
        if (xn && yn) {
            // Compare numerically without overflow: shorter digit string is smaller (no leading zeros).
            if (auto c = x.size() <=> y.size(); c != 0) return c;
            if (auto c = x <=> y; c != 0) return c;
        } else if (xn != yn) {
            return xn ? std::strong_ordering::less : std::strong_ordering::greater;
        } else if (auto c = x <=> y; c != 0) {
            return c;
        }
    }
    return a.pre_.size() <=> b.pre_.size();
}

void VersionRange::appendComparators(std::string_view token, Set& out) {
    auto take = [&token](std::string_view prefix) {
        if (token.substr(0, prefix.size()) == prefix) {
            token.remove_prefix(prefix.size());
            return true;
        }
        return false;
    };
    if (take(">=")) {
        out.push_back({Op::Ge, Version::parse(token)});
    } else if (take("<=")) {
        out.push_back({Op::Le, Version::parse(token)});
    } else if (take(">")) {
        out.push_back({Op::Gt, Version::parse(token)});
    } else if (take("<")) {
        out.push_back({Op::Lt, Version::parse(token)});
    } else if (take("=")) {
        out.push_back({Op::Eq, Version::parse(token)});
    } else if (take("^")) {
        const Version v = Version::parse(token);
        Version upper;
        if (v.major() > 0) {
            upper = Version(v.major() + 1, 0, 0);
        } else if (v.minor() > 0) {
            upper = Version(0, v.minor() + 1, 0);
        } else {
            upper = Version(0, 0, v.patch() + 1);
        }
        out.push_back({Op::Ge, v});
        out.push_back({Op::Lt, upper});
    } else if (take("~")) {
        const Version v = Version::parse(token);
        out.push_back({Op::Ge, v});
        out.push_back({Op::Lt, Version(v.major(), v.minor() + 1, 0)});
    } else {
        out.push_back({Op::Eq, Version::parse(token)});
    }
}

VersionRange::Set VersionRange::parseSet(std::string_view alternative) {
    Set set;
    for (std::string_view tok : split(alternative, ' ')) {
        tok = trim(tok);
        if (!tok.empty()) appendComparators(tok, set);
    }
    return set;
}

VersionRange VersionRange::parse(std::string_view text) {
    VersionRange r;
    r.text_ = std::string(trim(text));
    std::size_t start = 0;
    const std::string& t = r.text_;
    for (;;) {
        const std::size_t bar = t.find("||", start);
        const std::string_view alt = trim(std::string_view(t).substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        Set set;
        if (!alt.empty() && alt != "*") {
            set = parseSet(alt);
        } else if (alt.empty() && (bar != std::string::npos || start != 0)) {
            throw std::invalid_argument("empty alternative in range: " + t);
        }
        r.sets_.push_back(std::move(set));
        if (bar == std::string::npos) break;
        start = bar + 2;
    }
    return r;
}

bool VersionRange::contains(const Version& v) const {
    for (const Set& set : sets_) {
        bool ok = true;
        bool prereleaseAllowed = !v.isPrerelease();
        for (const Comparator& c : set) {
            const auto cmp = v <=> c.version;
            switch (c.op) {
                case Op::Eq: ok = ok && cmp == 0; break;
                case Op::Gt: ok = ok && cmp > 0; break;
                case Op::Ge: ok = ok && cmp >= 0; break;
                case Op::Lt: ok = ok && cmp < 0; break;
                case Op::Le: ok = ok && cmp <= 0; break;
            }
            if (c.version.isPrerelease() && c.version.major() == v.major() && c.version.minor() == v.minor() &&
                c.version.patch() == v.patch()) {
                prereleaseAllowed = true;
            }
        }
        if (ok && prereleaseAllowed) return true;
    }
    return false;
}

}  // namespace rfmod
