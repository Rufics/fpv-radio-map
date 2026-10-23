// The package check (rfmod/package.hpp) on raw bytes, as `rfmod install` or a module root
// file delivers them: gzip and tar parsing (libarchive), path safety, the size limits and the
// META/signature document. The signature stops anything else, see signed_package_fuzzer.
//
// Input: the .rfmod file. Property: verify() never throws, and only accepts signed packages.

#include "../support.hpp"

#include <rfmod/package.hpp>

#include <filesystem>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const rfmod::Version host = rfmod::Version::parse(RFMAP_HOST_VERSION);
    // A store file that does not exist: only the built-in key is trusted.
    static const rfmod::TrustStore trust(RFMAP_OWNER_PUBKEY_TEXT,
                                         rfmap::fuzz::noSuchKeysFile());
    // Small limits keep each run fast and let the fuzzer reach them.
    rfmod::PackageLimits limits;
    limits.maxPackageBytes = 1u << 20;
    limits.maxUnpackedBytes = 4u << 20;
    limits.maxFiles = 64;

    const std::string archive = rfmap::fuzz::asText(data, size);
    rfmod::ValidationReport report;
    const auto verified = rfmod::PackageVerifier(trust, host, limits).verify(archive, report);
    rfmap::fuzz::check(verified.has_value() == report.ok());
    return 0;
}
