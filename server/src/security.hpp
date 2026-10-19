#pragma once

// Secrets handling. Tokens (sessions, owner keys, link tokens) are 256-bit random values, so a
// plain SHA-256 is enough to store them: nobody can search that space. PINs are six digits, so
// they get a slow, salted argon2id hash instead.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace rfmap {

using Digest = std::array<std::uint8_t, 32>;

class Secrets {
public:
    /// Initialises libsodium once; safe to call from anywhere.
    static void init();
    /// `bytes` of CSPRNG output as unpadded base64url.
    static std::string randomToken(std::size_t bytes = 32);
    static Digest sha256(std::string_view data);
    static std::string hex(const std::uint8_t* data, std::size_t size);
    static std::string hex(const Digest& d) { return hex(d.data(), d.size()); }
    /// Constant-time comparison for strings of any length: both sides are hashed first, so
    /// neither the content nor the length leaks through timing.
    static bool constantTimeEqual(std::string_view a, std::string_view b);
    static bool constantTimeEqual(const Digest& a, const Digest& b);
    /// Keyed MAC (HMAC-SHA-512-256) as hex, for short-lived signed URLs.
    static std::string mac(std::string_view key, std::string_view message);
};

/// argon2id via libsodium crypto_pwhash_str. The cost is configurable so tests run quickly.
class PinHasher {
public:
    enum class Cost { Interactive, Moderate, Sensitive };
    explicit PinHasher(Cost cost = Cost::Moderate);

    [[nodiscard]] std::string hash(std::string_view pin) const;
    [[nodiscard]] bool verify(const std::string& stored, std::string_view pin) const;

    static Cost costFromName(std::string_view name);

private:
    unsigned long long opsLimit_;
    std::size_t memLimit_;
};

}  // namespace rfmap
