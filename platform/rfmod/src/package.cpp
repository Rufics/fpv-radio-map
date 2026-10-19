#include "rfmod/package.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rfmod {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kFileList = "META/files.sha256";
constexpr const char* kSignature = "META/signature";

bool safePath(std::string_view p) {
    if (p.empty() || p.front() == '/' || p.find('\\') != std::string_view::npos ||
        p.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t slash = p.find('/', start);
        const std::size_t length = slash == std::string_view::npos ? std::string_view::npos : slash - start;
        if (const std::string_view part = p.substr(start, length); part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

/// libarchive's message for the last error, which is null when it set none.
std::string archiveError(archive* a) {
    const char* message = archive_error_string(a);
    return message ? message : "unknown error";
}

struct ArchiveReadDeleter {
    void operator()(archive* a) const { archive_read_free(a); }
};
struct ArchiveWriteDeleter {
    void operator()(archive* a) const { archive_write_free(a); }
};

/// Path -> SHA-256 hex, as META/files.sha256 lists them.
using FileHashes = std::map<std::string, std::string, std::less<>>;

FileHashes parseFileList(const std::string& text, std::string& error) {
    FileHashes out;
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

/// Unpacks the entries of one open archive, enforcing the package limits as it goes. Each refusal
/// is a failed check in the report.
class ArchiveUnpacker {
public:
    ArchiveUnpacker(archive* source, const PackageLimits& limits, ValidationReport& report)
        : source_(source), limits_(limits), report_(&report) {}

    /// Every regular file in the archive, or nullopt after the first refusal.
    std::optional<PackageFiles> unpack() {
        archive_entry* entry = nullptr;
        int rc = ARCHIVE_OK;
        while ((rc = archive_read_next_header(source_, &entry)) == ARCHIVE_OK) {
            if (!add(entry)) return std::nullopt;
        }
        if (rc != ARCHIVE_EOF) {
            refuse("archive.format", "corrupt archive: " + archiveError(source_));
            return std::nullopt;
        }
        report_->pass("archive", std::format("{} files, {} bytes unpacked", files_.files.size(), total_));
        return std::move(files_);
    }

private:
    /// Adds one entry, skipping directories. False when the entry is refused.
    bool add(archive_entry* entry) {
        const char* name = archive_entry_pathname(entry);
        std::string path = name ? name : "";
        if (path.starts_with("./")) path.erase(0, 2);
        if (archive_entry_filetype(entry) == AE_IFDIR) return true;
        if (!acceptable(entry, path)) return false;
        auto content = readContent(path);
        if (!content) return false;
        files_.files.try_emplace(std::move(path), std::move(*content));
        return true;
    }

    /// A regular file at a safe, new path, within the file count limit.
    bool acceptable(archive_entry* entry, const std::string& path) {
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) != nullptr) {
            return refuse("archive.entries", "only regular files are allowed: " + path);
        }
        if (!safePath(path)) return refuse("archive.paths", "unsafe path: " + path);
        if (files_.files.contains(path)) return refuse("archive.entries", "duplicate entry: " + path);
        if (files_.files.size() + 1 > limits_.maxFiles) {
            return refuse("archive.size", std::format("more than {} files", limits_.maxFiles));
        }
        return true;
    }

    /// The current entry's bytes, counted against the unpacked size limit.
    std::optional<std::string> readContent(const std::string& path) {
        std::string content;
        std::array<char, 65536> buffer{};
        la_ssize_t n = 0;
        while ((n = archive_read_data(source_, buffer.data(), buffer.size())) > 0) {
            total_ += static_cast<std::size_t>(n);
            if (total_ > limits_.maxUnpackedBytes) {
                refuse("archive.size", std::format("unpacked size over {} bytes", limits_.maxUnpackedBytes));
                return std::nullopt;
            }
            content.append(buffer.data(), static_cast<std::size_t>(n));
        }
        if (n < 0) {
            refuse("archive.format", std::format("corrupt entry {}: {}", path, archiveError(source_)));
            return std::nullopt;
        }
        return content;
    }

    bool refuse(std::string check, std::string detail) {
        report_->fail(std::move(check), std::move(detail));
        return false;
    }

    archive* source_;
    PackageLimits limits_;
    ValidationReport* report_;
    PackageFiles files_;
    std::size_t total_ = 0;
};

/// The checks PackageVerifier::verify runs on one unpacked package. The signature comes first:
/// nothing else in the package is trusted before it checks out.
class SignedPackageCheck {
public:
    SignedPackageCheck(PackageFiles files, ValidationReport& report) : files_(std::move(files)), report_(&report) {}

    /// The trusted key whose signature covers the file list, or nullopt.
    std::optional<TrustedKey> signer(const TrustStore& trust) {
        const std::string* list = file(kFileList);
        const std::string* signature = file(kSignature);
        if (!list || !signature) {
            refuse("signature", "package is not signed (META/files.sha256 and META/signature are required)");
            return std::nullopt;
        }
        try {
            return verifiedSigner(trust, *list, json::parse(*signature));
        } catch (const json::exception& e) {
            refuse("signature", std::string("unreadable META/signature: ") + e.what());
        } catch (const std::invalid_argument& e) {
            refuse("signature", std::string("unreadable META/signature: ") + e.what());
        }
        return std::nullopt;
    }

    /// The signed file list and the files match both ways, byte for byte.
    bool integrity() {
        std::string listError;
        const auto listed = parseFileList(*file(kFileList), listError);
        if (!listError.empty()) return refuse("files.integrity", listError);
        for (const auto& [path, content] : files_.files) {
            if (!coveredBySignature(listed, path, content)) return false;
        }
        for (const auto& [path, hash] : listed) {
            if (!files_.files.contains(path)) return refuse("files.integrity", path + " is listed but missing");
        }
        report_->pass("files.integrity", std::format("{} files match their SHA-256", listed.size()));
        return true;
    }

    /// manifest.json, parsed and checked against the host version.
    std::optional<Manifest> manifest(const Version& hostVersion) {
        const std::string* text = file("manifest.json");
        if (!text) {
            refuse("manifest.schema", "manifest.json is missing");
            return std::nullopt;
        }
        json document;
        try {
            document = json::parse(*text);
        } catch (const json::exception& e) {
            refuse("manifest.schema", std::string("manifest.json is not JSON: ") + e.what());
            return std::nullopt;
        }
        return ManifestParser::parse(document, hostVersion, *report_);
    }

    /// Every file the manifest names is in the package, and its options schema is JSON.
    bool referencedFiles(const Manifest& manifest) {
        std::vector<std::string> referenced;
        referenced.reserve(manifest.entry.size() + 2);
        for (const auto& [platform, lib] : manifest.entry) referenced.push_back(lib);
        if (manifest.ui) referenced.push_back(manifest.ui->bundle);
        if (manifest.optionsSchema) referenced.push_back(*manifest.optionsSchema);
        std::string missing;
        for (const std::string& r : referenced) {
            if (!files_.files.contains(r)) missing += (missing.empty() ? "" : ", ") + r;
        }
        if (!missing.empty()) return refuse("files.referenced", "manifest names files the package lacks: " + missing);
        if (manifest.optionsSchema && !json::accept(*file(*manifest.optionsSchema))) {
            return refuse("files.referenced", *manifest.optionsSchema + " is not JSON");
        }
        report_->pass("files.referenced", "every file named in the manifest is present");
        return true;
    }

    /// The checked files, handed over once every check passed.
    PackageFiles take() && { return std::move(files_); }

private:
    std::optional<TrustedKey> verifiedSigner(const TrustStore& trust, const std::string& list, const json& sig) {
        if (sig.at("alg").get<std::string>() != "ed25519") throw std::invalid_argument("unsupported algorithm");
        const std::string keyId = sig.at("keyId").get<std::string>();
        auto signer = trust.find(keyId);
        if (!signer) {
            refuse("signature", "signed by key " + keyId + ", which this server does not trust");
            return std::nullopt;
        }
        if (!signer->key.verify(list, fromBase64(sig.at("signature").get<std::string>()))) {
            refuse("signature", "signature does not match the file list");
            return std::nullopt;
        }
        report_->pass("signature", std::format("valid ed25519 signature by {} ({}){}", signer->label, keyId,
                                               signer->firstParty() ? ", first-party" : ", third-party: runs isolated"));
        return signer;
    }

    /// One packed file: META/ holds only the list and the signature, and every other file is listed
    /// with its hash.
    bool coveredBySignature(const FileHashes& listed, const std::string& path,
                            const std::string& content) {
        if (path.starts_with("META/")) {
            return path == kFileList || path == kSignature || refuse("files.integrity", "unexpected file " + path);
        }
        const auto it = listed.find(path);
        if (it == listed.end()) return refuse("files.integrity", path + " is not covered by the signature");
        if (it->second != Sha256::hex(content)) return refuse("files.integrity", path + " was changed after signing");
        return true;
    }

    [[nodiscard]] const std::string* file(const std::string& path) const {
        const auto it = files_.files.find(path);
        return it == files_.files.end() ? nullptr : &it->second;
    }

    bool refuse(std::string check, std::string detail) {
        report_->fail(std::move(check), std::move(detail));
        return false;
    }

    PackageFiles files_;
    ValidationReport* report_;
};

}  // namespace

std::optional<PackageFiles> PackageReader::read(std::string_view archiveBytes, const PackageLimits& limits,
                                                ValidationReport& report) {
    if (archiveBytes.size() > limits.maxPackageBytes) {
        report.fail("archive.size",
                    std::format("package is {} bytes, limit {}", archiveBytes.size(), limits.maxPackageBytes));
        return std::nullopt;
    }
    std::unique_ptr<archive, ArchiveReadDeleter> a(archive_read_new());
    archive_read_support_filter_gzip(a.get());
    archive_read_support_format_tar(a.get());
    if (archive_read_open_memory(a.get(), archiveBytes.data(), archiveBytes.size()) != ARCHIVE_OK) {
        report.fail("archive.format", "not a gzip tar: " + archiveError(a.get()));
        return std::nullopt;
    }
    return ArchiveUnpacker(a.get(), limits, report).unpack();
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
        files.files.try_emplace(path, content);
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
        throw PackageWriteError("archive: " + archiveError(a.get()));
    }
    archive_write_set_format_pax_restricted(a.get());
    std::string out;
    // libarchive's write callback type fixes the untyped client data and buffer pointers.
    auto writeCb = [](archive*, void* ctx, const void* buf, std::size_t len) {  // NOSONAR(cpp:S5008): libarchive API
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(buf), len);
        return static_cast<la_ssize_t>(len);
    };
    // Writing to memory needs no open or close step.
    if (archive_write_open(a.get(), &out, nullptr, writeCb, nullptr) != ARCHIVE_OK) {
        throw PackageWriteError("archive: " + archiveError(a.get()));
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
            throw PackageWriteError("archive: " + archiveError(a.get()));
        }
        archive_entry_free(e);
    }
    if (archive_write_close(a.get()) != ARCHIVE_OK) {
        throw PackageWriteError("archive: " + archiveError(a.get()));
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
        files.files.try_emplace(rel, content.str());
    }
    return pack(files, key);
}

std::optional<VerifiedPackage> PackageVerifier::verify(const std::string& archiveBytes, const TrustStore& trust,
                                                       const Version& hostVersion, ValidationReport& report,
                                                       const PackageLimits& limits) {
    auto files = PackageReader::read(archiveBytes, limits, report);
    if (!files) return std::nullopt;
    SignedPackageCheck check(std::move(*files), report);
    auto signer = check.signer(trust);
    if (!signer || !check.integrity()) return std::nullopt;
    auto manifest = check.manifest(hostVersion);
    if (!manifest || !check.referencedFiles(*manifest)) return std::nullopt;
    return VerifiedPackage{std::move(check).take(), std::move(*manifest), std::move(*signer)};
}

}  // namespace rfmod
