#pragma once

#include "config.hpp"
#include "security.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfmap {

/// Bearer tokens for scripts, CI and other services (services.tokens in the config): they run
/// calculations without a mission session. The configuration holds each token's SHA-256 only;
/// the tokens are 256-bit random values (`rfmap token new`), so the hash cannot be searched back.
class ServiceTokens {
public:
    struct Token {
        std::string name;
        Digest sha256{};
    };

    explicit ServiceTokens(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    /// The tokens of services.tokens, whose names and digests Config::fromJson has checked.
    static ServiceTokens fromConfig(const std::vector<Config::ServiceToken>& tokens);

    /// The name of the configured token `bearer` is, if any. Every entry is compared in constant
    /// time, without stopping at a match, so the timing tells nothing about the tokens.
    [[nodiscard]] std::optional<std::string> match(std::string_view bearer) const;
    [[nodiscard]] bool empty() const { return tokens_.empty(); }

    /// The mission id a service's principal carries: no mission has it (ids are base64url), so
    /// the coverage jobs a service starts are visible to that service only.
    [[nodiscard]] static std::string principalId(std::string_view name) { return "service:" + std::string(name); }

private:
    std::vector<Token> tokens_;
};

}  // namespace rfmap
