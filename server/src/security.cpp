#include "security.hpp"

#include "service_errors.hpp"

#include <sodium.h>

#include <array>
#include <charconv>
#include <ctime>
#include <format>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace rfmap {

void Secrets::init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) throw CryptoError("libsodium failed to initialise");
    });
}

std::string Secrets::randomToken(std::size_t bytes) {
    init();
    std::vector<unsigned char> raw(bytes);
    randombytes_buf(raw.data(), raw.size());
    std::string out(sodium_base64_ENCODED_LEN(bytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), raw.data(), raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::char_traits<char>::length(out.c_str()));
    sodium_memzero(raw.data(), raw.size());
    return out;
}

Digest Secrets::sha256(std::string_view data) {
    init();
    Digest d{};
    crypto_hash_sha256(d.data(), reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return d;
}

std::string Secrets::hex(const std::uint8_t* data, std::size_t size) {
    std::string out(size * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), data, size);
    out.pop_back();
    return out;
}

bool Secrets::constantTimeEqual(const Digest& a, const Digest& b) {
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool Secrets::constantTimeEqual(std::string_view a, std::string_view b) {
    return constantTimeEqual(sha256(a), sha256(b));
}

std::string Secrets::mac(std::string_view key, std::string_view message) {
    init();
    // The key is hashed to the MAC's key size, so any configured secret length works.
    const Digest k = sha256(key);
    std::array<unsigned char, crypto_auth_BYTES> out{};
    crypto_auth(out.data(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), k.data());
    return hex(out.data(), out.size());
}

PinHasher::PinHasher(Cost cost) {
    Secrets::init();
    using enum Cost;
    switch (cost) {
        case Interactive:
            opsLimit_ = crypto_pwhash_OPSLIMIT_INTERACTIVE;
            memLimit_ = crypto_pwhash_MEMLIMIT_INTERACTIVE;
            break;
        case Moderate:
            opsLimit_ = crypto_pwhash_OPSLIMIT_MODERATE;
            memLimit_ = crypto_pwhash_MEMLIMIT_MODERATE;
            break;
        case Sensitive:
            opsLimit_ = crypto_pwhash_OPSLIMIT_SENSITIVE;
            memLimit_ = crypto_pwhash_MEMLIMIT_SENSITIVE;
            break;
    }
}

PinHasher::Cost PinHasher::costFromName(std::string_view name) {
    using enum Cost;
    if (name == "interactive") return Interactive;
    if (name == "moderate") return Moderate;
    if (name == "sensitive") return Sensitive;
    throw std::invalid_argument("argon2 cost must be interactive, moderate or sensitive");
}

std::string PinHasher::hash(std::string_view pin) const {
    std::array<char, crypto_pwhash_STRBYTES> out{};
    if (crypto_pwhash_str_alg(out.data(), pin.data(), pin.size(), opsLimit_, memLimit_, crypto_pwhash_ALG_ARGON2ID13) !=
        0) {
        throw CryptoError("argon2id: out of memory");
    }
    return out.data();
}

bool PinHasher::verify(const std::string& stored, std::string_view pin) const {
    return crypto_pwhash_str_verify(stored.c_str(), pin.data(), pin.size()) == 0;
}

std::string BundleGrants::issue(std::string_view moduleId, TimePoint until) const {
    const std::string exp = std::to_string(Clock::to_time_t(until));
    return std::format("{}.{}", exp, Secrets::mac(secret_, std::format("{}|{}", moduleId, exp)));
}

bool BundleGrants::valid(std::string_view moduleId, std::string_view grant, TimePoint now) const {
    const auto dot = grant.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view exp = grant.substr(0, dot);
    std::time_t expiry = 0;
    if (const auto [end, ec] = std::from_chars(exp.data(), exp.data() + exp.size(), expiry);
        ec != std::errc{} || end != exp.data() + exp.size()) {
        return false;
    }
    if (Clock::from_time_t(expiry) < now) return false;
    return Secrets::constantTimeEqual(grant.substr(dot + 1), Secrets::mac(secret_, std::format("{}|{}", moduleId, exp)));
}

}  // namespace rfmap
