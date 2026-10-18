// The package upload check (rfmod/package.hpp) on raw bytes, as an admin upload or a registry
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
                                         std::filesystem::temp_directory_path() / "rfmap-fuzz-no-such-keys.json");
    // Small limits keep each run fast and let the fuzzer reach them.
    rfmod::PackageLimits limits;
    limits.maxPackageBytes = 1u << 20;
    limits.maxUnpackedBytes = 4u << 20;
    limits.maxFiles = 64;

    const std::string archive(reinterpret_cast<const char*>(data), size);
    rfmod::ValidationReport report;
    const auto verified = rfmod::PackageVerifier::verify(archive, trust, host, report, limits);
    RFMAP_FUZZ_CHECK(verified.has_value() == report.ok());
    return 0;
}
