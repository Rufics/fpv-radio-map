#include "security.hpp"

#include <sodium.h>

#include <mutex>
#include <stdexcept>
#include <vector>

namespace rfmap {

void Secrets::init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
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
    unsigned char out[crypto_auth_BYTES];
    crypto_auth(out, reinterpret_cast<const unsigned char*>(message.data()), message.size(), k.data());
    return hex(out, sizeof out);
}

PinHasher::PinHasher(Cost cost) {
    Secrets::init();
    switch (cost) {
        case Cost::Interactive:
            opsLimit_ = crypto_pwhash_OPSLIMIT_INTERACTIVE;
            memLimit_ = crypto_pwhash_MEMLIMIT_INTERACTIVE;
            break;
        case Cost::Moderate:
            opsLimit_ = crypto_pwhash_OPSLIMIT_MODERATE;
            memLimit_ = crypto_pwhash_MEMLIMIT_MODERATE;
            break;
        case Cost::Sensitive:
            opsLimit_ = crypto_pwhash_OPSLIMIT_SENSITIVE;
            memLimit_ = crypto_pwhash_MEMLIMIT_SENSITIVE;
            break;
    }
}

PinHasher::Cost PinHasher::costFromName(const std::string& name) {
    if (name == "interactive") return Cost::Interactive;
    if (name == "moderate") return Cost::Moderate;
    if (name == "sensitive") return Cost::Sensitive;
    throw std::invalid_argument("argon2 cost must be interactive, moderate or sensitive");
}

std::string PinHasher::hash(std::string_view pin) const {
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str_alg(out, pin.data(), pin.size(), opsLimit_, memLimit_, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("argon2id: out of memory");
    }
    return out;
}

bool PinHasher::verify(const std::string& stored, std::string_view pin) const {
    return crypto_pwhash_str_verify(stored.c_str(), pin.data(), pin.size()) == 0;
}

}  // namespace rfmap
