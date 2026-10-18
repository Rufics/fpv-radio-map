#include "rfmod/package.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <memory>
#include <ranges>
#include <sstream>
#include <stdexcept>

namespace rfmod {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kFileList = "META/files.sha256";
constexpr const char* kSignature = "META/signature";

bool safePath(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.find('\\') != std::string::npos || p.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t slash = p.find('/', start);
        const std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

struct ArchiveReadDeleter {
    void operator()(archive* a) const { archive_read_free(a); }
};
struct ArchiveWriteDeleter {
    void operator()(archive* a) const { archive_write_free(a); }
};

std::map<std::string, std::string> parseFileList(const std::string& text, std::string& error) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    std::string previous;
    while (std::getline(in, line)) {
        if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') {
            error = "malformed line in " + std::string(kFileList);
            return {};
        }
        const std::string hash = line.substr(0, 64);
        const std::string path = line.substr(66);
        if (hash.find_first_not_of("0123456789abcdef") != std::string::npos || !safePath(path)) {
            error = "bad entry in " + std::string(kFileList) + ": " + path;
            return {};
        }
        if (!previous.empty() && path <= previous) {
            error = std::string(kFileList) + " must be sorted and free of duplicates";
            return {};
        }
        previous = path;
        out[path] = hash;
    }
    if (!text.empty() && text.back() != '\n') {
        error = std::string(kFileList) + " must end with a newline";
        return {};
    }
    return out;
}

/// Checks one archive entry before its data is read: a regular file with a safe, new path,
/// within the file count limit.
bool acceptEntry(archive_entry* entry, const std::string& path, const PackageFiles& files, const PackageLimits& limits,
                 ValidationReport& report) {
    if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) != nullptr) {
        report.fail("archive.entries", "only regular files are allowed: " + path);
        return false;
    }
    if (!safePath(path)) {
        report.fail("archive.paths", "unsafe path: " + path);
        return false;
    }
    if (files.files.contains(path)) {
        report.fail("archive.entries", "duplicate entry: " + path);
        return false;
    }
    if (files.files.size() + 1 > limits.maxFiles) {
        report.fail("archive.size", "more than " + std::to_string(limits.maxFiles) + " files");
        return false;
    }
    return true;
}

/// Reads the current entry's data, adding its size to `total` (the whole archive's unpacked size).
std::optional<std::string> readEntryData(archive* a, const std::string& path, std::size_t& total,
                                         const PackageLimits& limits, ValidationReport& report) {
    std::string content;
    std::array<char, 65536> buf{};
    la_ssize_t n = 0;
    while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
        total += static_cast<std::size_t>(n);
        if (total > limits.maxUnpackedBytes) {
            report.fail("archive.size", "unpacked size over " + std::to_string(limits.maxUnpackedBytes) + " bytes");
            return std::nullopt;
        }
        content.append(buf.data(), static_cast<std::size_t>(n));
    }
    if (n < 0) {
        report.fail("archive.format", std::string("corrupt entry ") + path + ": " + archive_error_string(a));
        return std::nullopt;
    }
    return content;
}

/// Checks META/signature against the trust store; the signer if it vouches for `fileList`.
std::optional<TrustedKey> checkSignature(const std::string& signature, const std::string& fileList,
                                         const TrustStore& trust, ValidationReport& report) {
    try {
        const json sig = json::parse(signature);
        if (sig.at("alg").get<std::string>() != "ed25519") throw std::invalid_argument("unsupported algorithm");
        const std::string keyId = sig.at("keyId").get<std::string>();
        std::optional<TrustedKey> signer = trust.find(keyId);
        if (!signer.has_value()) {
            report.fail("signature", "signed by key " + keyId + ", which this server does not trust");
            return std::nullopt;
        }
        if (!signer->key.verify(fileList, fromBase64(sig.at("signature").get<std::string>()))) {
            report.fail("signature", "signature does not match the file list");
            return std::nullopt;
        }
        report.pass("signature", "valid ed25519 signature by " + signer->label + " (" + keyId + ")" +
                                     (signer->firstParty() ? ", first-party" : ", third-party: runs isolated"));
        return signer;
    } catch (const std::exception& e) {
        report.fail("signature", std::string("unreadable META/signature: ") + e.what());
        return std::nullopt;
    }
}

/// Checks that every file outside META/ is listed with its hash and every listed file exists.
bool checkIntegrity(const PackageFiles& files, const std::string& fileList, ValidationReport& report) {
    std::string listError;
    const auto listed = parseFileList(fileList, listError);
    if (!listError.empty()) {
        report.fail("files.integrity", listError);
        return false;
    }
    for (const auto& [path, content] : files.files) {
        if (path.starts_with("META/")) {
            if (path == kFileList || path == kSignature) continue;
            report.fail("files.integrity", "unexpected file " + path);
            return false;
        }
        const auto it = listed.find(path);
        if (it == listed.end()) {
            report.fail("files.integrity", path + " is not covered by the signature");
            return false;
        }
        if (it->second != Sha256::hex(content)) {
            report.fail("files.integrity", path + " was changed after signing");
            return false;
        }
    }
    for (const auto& [path, hash] : listed) {
        if (!files.files.contains(path)) {
            report.fail("files.integrity", path + " is listed but missing");
            return false;
        }
    }
    report.pass("files.integrity", std::to_string(listed.size()) + " files match their SHA-256");
    return true;
}

/// Parses manifest.json and checks it against the schema and rules.
std::optional<Manifest> readManifest(const PackageFiles& files, const Version& hostVersion, ValidationReport& report) {
    const auto manifestIt = files.files.find("manifest.json");
    if (manifestIt == files.files.end()) {
        report.fail("manifest.schema", "manifest.json is missing");
        return std::nullopt;
    }
    json document;
    try {
        document = json::parse(manifestIt->second);
    } catch (const std::exception& e) {
        report.fail("manifest.schema", std::string("manifest.json is not JSON: ") + e.what());
        return std::nullopt;
    }
    return ManifestParser::parse(document, hostVersion, report);
}

/// Checks that the libraries, UI bundle and options schema the manifest names are in the package.
bool checkReferencedFiles(const PackageFiles& files, const Manifest& manifest, ValidationReport& report) {
    std::vector<std::string> referenced;
    referenced.reserve(manifest.entry.size() + 2);
    for (const std::string& lib : std::views::values(manifest.entry)) referenced.push_back(lib);
    if (manifest.ui.has_value()) referenced.push_back(manifest.ui->bundle);
    if (manifest.optionsSchema.has_value()) referenced.push_back(*manifest.optionsSchema);
    std::string missing;
    for (const std::string& r : referenced) {
        if (!files.files.contains(r)) missing += (missing.empty() ? "" : ", ") + r;
    }
    if (!missing.empty()) {
        report.fail("files.referenced", "manifest names files the package lacks: " + missing);
        return false;
    }
    if (manifest.optionsSchema.has_value() && !json::accept(files.files.at(*manifest.optionsSchema))) {
        report.fail("files.referenced", *manifest.optionsSchema + " is not JSON");
        return false;
    }
    report.pass("files.referenced", "every file named in the manifest is present");
    return true;
}

}  // namespace

std::optional<PackageFiles> PackageReader::read(const std::string& archiveBytes, const PackageLimits& limits,
                                                ValidationReport& report) {
    if (archiveBytes.size() > limits.maxPackageBytes) {
        report.fail("archive.size", "package is " + std::to_string(archiveBytes.size()) + " bytes, limit " +
                                        std::to_string(limits.maxPackageBytes));
        return std::nullopt;
    }
    std::unique_ptr<archive, ArchiveReadDeleter> a(archive_read_new());
    archive_read_support_filter_gzip(a.get());
    archive_read_support_format_tar(a.get());
    if (archive_read_open_memory(a.get(), archiveBytes.data(), archiveBytes.size()) != ARCHIVE_OK) {
        report.fail("archive.format", std::string("not a gzip tar: ") + archive_error_string(a.get()));
        return std::nullopt;
    }
    PackageFiles out;
    std::size_t total = 0;
    archive_entry* entry = nullptr;
    int rc = ARCHIVE_OK;
    while ((rc = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
        if (path.starts_with("./")) path = path.substr(2);
        if (archive_entry_filetype(entry) == AE_IFDIR) continue;
        if (!acceptEntry(entry, path, out, limits, report)) return std::nullopt;
        auto content = readEntryData(a.get(), path, total, limits, report);
        if (!content) return std::nullopt;
        out.files.emplace(std::move(path), std::move(*content));
    }
    if (rc != ARCHIVE_EOF) {
        report.fail("archive.format", std::string("corrupt archive: ") + archive_error_string(a.get()));
        return std::nullopt;
    }
    report.pass("archive", std::to_string(out.files.size()) + " files, " + std::to_string(total) + " bytes unpacked");
    return out;
}

std::string PackageWriter::fileList(const PackageFiles& files) {
    std::string list;
    for (const auto& [path, content] : files.files) {
        if (path.starts_with("META/")) continue;
        list += Sha256::hex(content) + "  " + path + "\n";
    }
    return list;
}

std::string PackageWriter::pack(const PackageFiles& input, const SecretKey& key) {
    PackageFiles files;
    for (const auto& [path, content] : input.files) {
        if (path.starts_with("META/")) continue;
        if (!safePath(path)) throw std::invalid_argument("unsafe path: " + path);
        files.files.emplace(path, content);
    }
    if (!files.files.contains("manifest.json")) throw std::invalid_argument("package needs manifest.json");
    const std::string list = fileList(files);
    const Bytes sig = key.sign(list);
    files.files[kFileList] = list;
    files.files[kSignature] =
        json{{"alg", "ed25519"}, {"keyId", key.publicKey().id()}, {"signature", toBase64(sig.data(), sig.size())}}
            .dump() + "\n";

    std::unique_ptr<archive, ArchiveWriteDeleter> a(archive_write_new());
    archive_write_add_filter_gzip(a.get());
    // The gzip header stores the current time unless told not to, which makes packing depend on the clock.
    if (archive_write_set_filter_option(a.get(), "gzip", "timestamp", nullptr) != ARCHIVE_OK) {
        throw std::runtime_error(std::string("archive: ") + archive_error_string(a.get()));
    }
    archive_write_set_format_pax_restricted(a.get());
    std::string out;
    auto openCb = [](archive*, void*) -> int { return ARCHIVE_OK; };
    auto writeCb = [](archive*, void* ctx, const void* buf, std::size_t len) -> la_ssize_t {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(buf), len);
        return static_cast<la_ssize_t>(len);
    };
    auto closeCb = [](archive*, void*) -> int { return ARCHIVE_OK; };
    if (archive_write_open(a.get(), &out, openCb, writeCb, closeCb) != ARCHIVE_OK) {
        throw std::runtime_error(std::string("archive: ") + archive_error_string(a.get()));
    }
    for (const auto& [path, content] : files.files) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, path.c_str());
        archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_mtime(e, 0, 0);  // reproducible packages
        if (archive_write_header(a.get(), e) != ARCHIVE_OK ||
            archive_write_data(a.get(), content.data(), content.size()) != static_cast<la_ssize_t>(content.size())) {
            archive_entry_free(e);
            throw std::runtime_error(std::string("archive: ") + archive_error_string(a.get()));
        }
        archive_entry_free(e);
    }
    if (archive_write_close(a.get()) != ARCHIVE_OK) {
        throw std::runtime_error(std::string("archive: ") + archive_error_string(a.get()));
    }
    return out;
}

std::string PackageWriter::pack(const fs::path& sourceDir, const SecretKey& key) {
    PackageFiles files;
    for (const auto& item : fs::recursive_directory_iterator(sourceDir)) {
        if (item.is_symlink()) throw std::invalid_argument("symlinks are not allowed: " + item.path().string());
        if (!item.is_regular_file()) continue;
        const std::string rel = fs::relative(item.path(), sourceDir).generic_string();
        if (rel.starts_with("META/")) continue;
        std::ifstream in(item.path(), std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        files.files.emplace(rel, content.str());
    }
    return pack(files, key);
}

std::optional<VerifiedPackage> PackageVerifier::verify(const std::string& archiveBytes, const TrustStore& trust,
                                                       const Version& hostVersion, ValidationReport& report,
                                                       const PackageLimits& limits) {
    auto files = PackageReader::read(archiveBytes, limits, report);
    if (!files) return std::nullopt;

    const auto listIt = files->files.find(kFileList);
    const auto sigIt = files->files.find(kSignature);
    if (listIt == files->files.end() || sigIt == files->files.end()) {
        report.fail("signature", "package is not signed (META/files.sha256 and META/signature are required)");
        return std::nullopt;
    }

    // Signature first: nothing else in the package is trusted before it checks out.
    const std::optional<TrustedKey> signer = checkSignature(sigIt->second, listIt->second, trust, report);
    if (!signer.has_value()) return std::nullopt;
    if (!checkIntegrity(*files, listIt->second, report)) return std::nullopt;
    auto manifest = readManifest(*files, hostVersion, report);
    if (!manifest) return std::nullopt;
    if (!checkReferencedFiles(*files, *manifest, report)) return std::nullopt;

    return VerifiedPackage{std::move(*files), std::move(*manifest), *signer};
}

}  // namespace rfmod
