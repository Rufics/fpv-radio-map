#include "rfmod/crypto.hpp"

#include <sodium.h>

#include <stdexcept>

namespace rfmod {
namespace {

constexpr std::string_view kPublicPrefix = "rfmod-ed25519-pub:";
constexpr std::string_view kSecretPrefix = "rfmod-ed25519-secret:";

void ensureSodium() {
    static const bool ready = sodium_init() >= 0;
    if (!ready) throw std::runtime_error("libsodium failed to initialise");
}

std::string_view stripPrefix(std::string_view text, std::string_view prefix) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ')) text.remove_prefix(1);
    if (text.substr(0, prefix.size()) != prefix) {
        throw std::invalid_argument("key text must start with " + std::string(prefix));
    }
    return text.substr(prefix.size());
}

}  // namespace

std::array<std::uint8_t, 32> Sha256::digest(const void* data, std::size_t size) {
    ensureSodium();
    std::array<std::uint8_t, 32> out{};
    crypto_hash_sha256(out.data(), static_cast<const unsigned char*>(data), size);
    return out;
}

std::string Sha256::hex(const void* data, std::size_t size) {
    const auto d = digest(data, size);
    std::string out(d.size() * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), d.data(), d.size());
    out.pop_back();
    return out;
}

std::string toBase64(const std::uint8_t* data, std::size_t size) {
    ensureSodium();
    std::string out(sodium_base64_ENCODED_LEN(size, sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), data, size, sodium_base64_VARIANT_ORIGINAL);
    out.pop_back();
    return out;
}

Bytes fromBase64(std::string_view text) {
    ensureSodium();
    // An empty vector has no buffer, and libsodium requires one even for zero bytes.
    if (text.empty()) return {};
    Bytes out(text.size());
    std::size_t len = 0;
    const char* end = nullptr;
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &len, &end,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        end != text.data() + text.size()) {
        throw std::invalid_argument("invalid base64");
    }
    out.resize(len);
    return out;
}

PublicKey PublicKey::fromBytes(const Bytes& raw) {
    if (raw.size() != kSize) throw std::invalid_argument("Ed25519 public key must be 32 bytes");
    PublicKey k;
    std::copy(raw.begin(), raw.end(), k.key_.begin());
    return k;
}

PublicKey PublicKey::fromText(std::string_view text) { return fromBytes(fromBase64(stripPrefix(text, kPublicPrefix))); }

std::string PublicKey::toText() const { return std::string(kPublicPrefix) + toBase64(key_.data(), key_.size()); }

std::string PublicKey::id() const { return Sha256::hex(key_.data(), key_.size()).substr(0, 16); }

bool PublicKey::verify(std::string_view message, const Bytes& signature) const {
    ensureSodium();
    if (signature.size() != crypto_sign_BYTES) return false;
    return crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(message.data()),
                                       message.size(), key_.data()) == 0;
}

SecretKey SecretKey::generate() {
    ensureSodium();
    SecretKey k;
    std::array<std::uint8_t, PublicKey::kSize> pk{};
    crypto_sign_keypair(pk.data(), k.key_.data());
    return k;
}

SecretKey SecretKey::fromText(std::string_view text) {
    const Bytes raw = fromBase64(stripPrefix(text, kSecretPrefix));
    if (raw.size() != kSize) throw std::invalid_argument("Ed25519 secret key must be 64 bytes");
    SecretKey k;
    std::copy(raw.begin(), raw.end(), k.key_.begin());
    return k;
}

SecretKey::~SecretKey() { sodium_memzero(key_.data(), key_.size()); }
SecretKey::SecretKey(const SecretKey& other) : key_(other.key_) {}
SecretKey& SecretKey::operator=(const SecretKey& other) {
    key_ = other.key_;
    return *this;
}

std::string SecretKey::toText() const { return std::string(kSecretPrefix) + toBase64(key_.data(), key_.size()); }

PublicKey SecretKey::publicKey() const {
    Bytes pk(PublicKey::kSize);
    crypto_sign_ed25519_sk_to_pk(pk.data(), key_.data());
    return PublicKey::fromBytes(pk);
}

Bytes SecretKey::sign(std::string_view message) const {
    ensureSodium();
    Bytes sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                         key_.data());
    return sig;
}

}  // namespace rfmod
