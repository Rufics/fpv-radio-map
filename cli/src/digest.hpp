#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rfmap::cli {

/// OpenSSL could not hash or draw random bytes: exit status 70.
class CryptoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// SHA-256 over data that arrives in pieces, as lowercase hex.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) = delete;
    Sha256& operator=(Sha256&&) = delete;

    /// Starts over, forgetting what was added.
    void restart();
    /// False when OpenSSL failed; the digest is then of no use.
    [[nodiscard]] bool add(std::string_view data) noexcept;
    /// The digest of what was added since the last restart.
    [[nodiscard]] std::string hex();

    [[nodiscard]] static std::string of(std::string_view text);

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// `bytes` as lowercase hex, two digits each.
[[nodiscard]] std::string toHex(std::span<const std::byte> bytes);

}  // namespace rfmap::cli
