#include "service_token.hpp"

#include "digest.hpp"

#include <openssl/rand.h>

#include <array>

namespace rfmap::cli {

ServiceToken ServiceToken::generate() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw CryptoError("the random generator failed");
    }
    ServiceToken t;
    t.token = std::string(kPrefix) + base64url(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    t.sha256 = sha256Hex(t.token);
    return t;
}

std::string ServiceToken::sha256Hex(std::string_view text) { return Sha256::of(text); }

std::string ServiceToken::base64url(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto at = [&bytes](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(bytes[i])); };
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const unsigned n = at(i) << 16U | at(i + 1) << 8U | at(i + 2);
        out += kAlphabet[n >> 18U & 63U];
        out += kAlphabet[n >> 12U & 63U];
        out += kAlphabet[n >> 6U & 63U];
        out += kAlphabet[n & 63U];
    }
    // The last one or two bytes: two or three characters, no padding.
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        const unsigned n = at(i) << 16U | (rest == 2 ? at(i + 1) << 8U : 0U);
        out += kAlphabet[n >> 18U & 63U];
        out += kAlphabet[n >> 12U & 63U];
        if (rest == 2) out += kAlphabet[n >> 6U & 63U];
    }
    return out;
}

}  // namespace rfmap::cli
