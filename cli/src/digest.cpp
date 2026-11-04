#include "digest.hpp"

#include <openssl/evp.h>

#include <array>

namespace rfmap::cli {

struct Sha256::State {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
};

Sha256::Sha256() : state_(std::make_unique<State>()) { restart(); }

Sha256::~Sha256() = default;

void Sha256::restart() {
    if (!state_->ctx || EVP_DigestInit_ex(state_->ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw CryptoError("SHA-256 cannot start");
    }
}

bool Sha256::add(std::string_view data) noexcept {
    return EVP_DigestUpdate(state_->ctx.get(), data.data(), data.size()) == 1;
}

std::string Sha256::hex() {
    std::array<std::byte, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(state_->ctx.get(), reinterpret_cast<unsigned char*>(digest.data()), &size) != 1 ||
        size != 32) {
        throw CryptoError("SHA-256 failed");
    }
    return toHex(std::span(digest).first(size));
}

std::string Sha256::of(std::string_view text) {
    Sha256 h;
    if (!h.add(text)) throw CryptoError("SHA-256 failed");
    return h.hex();
}

std::string toHex(std::span<const std::byte> bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(2 * bytes.size());
    for (const std::byte b : bytes) {
        out += kHex[std::to_integer<unsigned>(b >> 4U)];
        out += kHex[std::to_integer<unsigned>(b & std::byte{0x0f})];
    }
    return out;
}

}  // namespace rfmap::cli
