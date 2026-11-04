#include "service_token.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <stdexcept>

namespace rfmap::cli {

ServiceToken ServiceToken::generate() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("the random generator failed");
    }
    ServiceToken t;
    t.token = std::string(kPrefix) + base64url(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    t.sha256 = sha256Hex(t.token);
    return t;
}

std::string ServiceToken::sha256Hex(std::string_view text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32) {
        throw std::runtime_error("SHA-256 failed");
    }
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(2 * static_cast<std::size_t>(size));
    for (unsigned int i = 0; i < size; ++i) {
        out += kHex[digest[i] >> 4U];
        out += kHex[digest[i] & 0x0fU];
    }
    return out;
}

std::string ServiceToken::base64url(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto n = static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) << 16U |
                       static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 1])) << 8U |
                       static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 2]));
        out += kAlphabet[n >> 18U & 63U];
        out += kAlphabet[n >> 12U & 63U];
        out += kAlphabet[n >> 6U & 63U];
        out += kAlphabet[n & 63U];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        unsigned n = static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) << 16U;
        if (rest == 2) n |= static_cast<unsigned>(static_cast<unsigned char>(bytes[i + 1])) << 8U;
        out += kAlphabet[n >> 18U & 63U];
        out += kAlphabet[n >> 12U & 63U];
        if (rest == 2) out += kAlphabet[n >> 6U & 63U];
    }
    return out;
}

}  // namespace rfmap::cli
