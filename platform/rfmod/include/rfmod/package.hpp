#pragma once

#include "rfmod/crypto.hpp"
#include "rfmod/manifest.hpp"
#include "rfmod/report.hpp"
#include "rfmod/trust.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace rfmod {

/// A .rfmod package is a gzip-compressed tar:
///
///   manifest.json
///   lib/...  ui/...  schema/...  docs/...
///   META/files.sha256   "<sha256 hex>  <path>\n" for every file outside META/, sorted by path
///   META/signature      {"alg":"ed25519","keyId":"...","signature":"<base64>"}
///
/// The signature covers the exact bytes of META/files.sha256, so one signature vouches for
/// every byte of the package.
struct PackageFiles {
    std::map<std::string, std::string> files;  ///< path -> content (binary-safe)
};

struct PackageLimits {
    std::size_t maxPackageBytes = 64u << 20;
    std::size_t maxUnpackedBytes = 256u << 20;
    std::size_t maxFiles = 2000;
};

class PackageReader {
public:
    /// Unpacks in memory. Refuses absolute paths, "..", links, devices and anything over the limits;
    /// each refusal is a failed check in `report`.
    static std::optional<PackageFiles> read(const std::string& archive, const PackageLimits& limits,
                                            ValidationReport& report);
};

class PackageWriter {
public:
    /// Packs every regular file under `sourceDir` (which must contain manifest.json) and signs it.
    static std::string pack(const std::filesystem::path& sourceDir, const SecretKey& key);
    /// Same for files already in memory; META/ entries in `files` are replaced.
    static std::string pack(const PackageFiles& files, const SecretKey& key);

    static std::string fileList(const PackageFiles& files);
};

struct VerifiedPackage {
    PackageFiles files;
    Manifest manifest;
    TrustedKey signer;
};

class PackageVerifier {
public:
    /// The full upload check: archive safety, file list, signature by a trusted key, manifest
    /// schema and rules, and every file the manifest references.
    static std::optional<VerifiedPackage> verify(const std::string& archive, const TrustStore& trust,
                                                 const Version& hostVersion, ValidationReport& report,
                                                 const PackageLimits& limits = {});
};

}  // namespace rfmod
