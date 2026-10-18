// Key and signature text (rfmod/crypto.hpp): base64 from META/signature of every uploaded
// package, and public keys the admin pastes into the trust store.
//
// Properties: decoded base64 encodes and decodes back to the same bytes, and a parsed key prints
// back to itself.

#include "../support.hpp"

#include <rfmod/crypto.hpp>

#include <stdexcept>

using namespace rfmod;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text = rfmap::fuzz::asText(data, size);

    try {
        const Bytes raw = fromBase64(text);
        RFMAP_FUZZ_CHECK(raw.size() <= text.size());
        RFMAP_FUZZ_CHECK(fromBase64(toBase64(raw.data(), raw.size())) == raw);
    } catch (const std::invalid_argument&) {
    }

    try {
        const PublicKey key = PublicKey::fromText(text);
        RFMAP_FUZZ_CHECK(PublicKey::fromText(key.toText()) == key);
        RFMAP_FUZZ_CHECK(key.id().size() == 16);
        // Garbage signatures of any length are refused, never read past their end.
        (void)key.verify(text, Bytes(text.begin(), text.end()));
    } catch (const std::invalid_argument&) {
    }

    try {
        const SecretKey key = SecretKey::fromText(text);
        RFMAP_FUZZ_CHECK(SecretKey::fromText(key.toText()).publicKey() == key.publicKey());
    } catch (const std::invalid_argument&) {
    }
    return 0;
}
