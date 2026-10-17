#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rfmod {

using Bytes = std::vector<std::uint8_t>;

/// SHA-256 helpers (libsodium).
struct Sha256 {
    static std::array<std::uint8_t, 32> digest(const void* data, std::size_t size);
    static std::string hex(const void* data, std::size_t size);
    static std::string hex(const Bytes& data) { return hex(data.data(), data.size()); }
    static std::string hex(std::string_view data) { return hex(data.data(), data.size()); }
};

std::string toBase64(const std::uint8_t* data, std::size_t size);
Bytes fromBase64(std::string_view text);  ///< Throws std::invalid_argument.

/// An Ed25519 public key. Text form: "rfmod-ed25519-pub:<base64>".
class PublicKey {
public:
    static constexpr std::size_t kSize = 32;
    static PublicKey fromText(std::string_view text);  ///< Throws std::invalid_argument.
    static PublicKey fromBytes(const Bytes& raw);

    [[nodiscard]] std::string toText() const;
    /// Short stable id: first 16 hex digits of SHA-256 over the raw key.
    [[nodiscard]] std::string id() const;
    [[nodiscard]] bool verify(std::string_view message, const Bytes& signature) const;
    [[nodiscard]] const std::array<std::uint8_t, kSize>& raw() const { return key_; }

    friend bool operator==(const PublicKey& a, const PublicKey& b) { return a.key_ == b.key_; }

private:
    std::array<std::uint8_t, kSize> key_{};
};

/// An Ed25519 secret key. Text form: "rfmod-ed25519-secret:<base64 of the 64-byte libsodium key>".
/// Keep it off the server: packages are signed on the author's machine.
class SecretKey {
public:
    static constexpr std::size_t kSize = 64;
    static SecretKey generate();
    static SecretKey fromText(std::string_view text);  ///< Throws std::invalid_argument.
    ~SecretKey();
    SecretKey(const SecretKey& other);
    SecretKey& operator=(const SecretKey& other);

    [[nodiscard]] std::string toText() const;
    [[nodiscard]] PublicKey publicKey() const;
    [[nodiscard]] Bytes sign(std::string_view message) const;

private:
    SecretKey() = default;
    std::array<std::uint8_t, kSize> key_{};
};

}  // namespace rfmod
