#include "service_tokens.hpp"

#include <stdexcept>

namespace rfmap {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    throw std::invalid_argument("not a lowercase hex digit");
}

/// 64 lowercase hex digits as a digest; Config::fromJson has checked the form.
Digest digestFromHex(std::string_view hex) {
    if (hex.size() != 64) throw std::invalid_argument("a SHA-256 is 64 hex digits");
    Digest d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = static_cast<std::uint8_t>(hexDigit(hex[2 * i]) * 16 + hexDigit(hex[2 * i + 1]));
    }
    return d;
}

}  // namespace

ServiceTokens ServiceTokens::fromConfig(const std::vector<Config::ServiceToken>& tokens) {
    std::vector<Token> out;
    out.reserve(tokens.size());
    for (const Config::ServiceToken& t : tokens) out.emplace_back(t.name, digestFromHex(t.sha256));
    return ServiceTokens(std::move(out));
}

std::optional<std::string> ServiceTokens::match(std::string_view bearer) const {
    if (bearer.empty() || tokens_.empty()) return std::nullopt;
    const Digest presented = Secrets::sha256(bearer);
    std::optional<std::string> found;
    for (const Token& t : tokens_) {
        if (Secrets::constantTimeEqual(presented, t.sha256) && !found) found = t.name;
    }
    return found;
}

}  // namespace rfmap
