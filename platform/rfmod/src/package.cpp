#include "rfmod/package.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
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
        report.fail("archive.format", std::string("not a gzip tar: ") + archiveError(a.get()));
        return std::nullopt;
    }
    PackageFiles out;
    std::size_t total = 0;
    archive_entry* entry = nullptr;
    int rc = ARCHIVE_OK;
    while ((rc = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
        if (path.rfind("./", 0) == 0) path = path.substr(2);
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            continue;
        }
        if (type != AE_IFREG || archive_entry_hardlink(entry) != nullptr) {
            report.fail("archive.entries", "only regular files are allowed: " + path);
            return std::nullopt;
        }
        if (!safePath(path)) {
            report.fail("archive.paths", "unsafe path: " + path);
            return std::nullopt;
        }
        if (out.files.count(path)) {
            report.fail("archive.entries", "duplicate entry: " + path);
            return std::nullopt;
        }
        if (out.files.size() + 1 > limits.maxFiles) {
            report.fail("archive.size", "more than " + std::to_string(limits.maxFiles) + " files");
            return std::nullopt;
        }
        std::string content;
        char buf[65536];
        la_ssize_t n = 0;
        while ((n = archive_read_data(a.get(), buf, sizeof buf)) > 0) {
            total += static_cast<std::size_t>(n);
            if (total > limits.maxUnpackedBytes) {
                report.fail("archive.size", "unpacked size over " + std::to_string(limits.maxUnpackedBytes) + " bytes");
                return std::nullopt;
            }
            content.append(buf, static_cast<std::size_t>(n));
        }
        if (n < 0) {
            report.fail("archive.format", std::string("corrupt entry ") + path + ": " + archiveError(a.get()));
            return std::nullopt;
        }
        out.files.emplace(std::move(path), std::move(content));
    }
    if (rc != ARCHIVE_EOF) {
        report.fail("archive.format", std::string("corrupt archive: ") + archiveError(a.get()));
        return std::nullopt;
    }
    report.pass("archive", std::to_string(out.files.size()) + " files, " + std::to_string(total) + " bytes unpacked");
    return out;
}

std::string PackageWriter::fileList(const PackageFiles& files) {
    std::string list;
    for (const auto& [path, content] : files.files) {
        if (path.rfind("META/", 0) == 0) continue;
        list += Sha256::hex(content) + "  " + path + "\n";
    }
    return list;
}

std::string PackageWriter::pack(const PackageFiles& input, const SecretKey& key) {
    PackageFiles files;
    for (const auto& [path, content] : input.files) {
        if (path.rfind("META/", 0) == 0) continue;
        if (!safePath(path)) throw std::invalid_argument("unsafe path: " + path);
        files.files.emplace(path, content);
    }
    if (!files.files.count("manifest.json")) throw std::invalid_argument("package needs manifest.json");
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
        throw std::runtime_error("archive: " + archiveError(a.get()));
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
        throw std::runtime_error("archive: " + archiveError(a.get()));
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
            throw std::runtime_error("archive: " + archiveError(a.get()));
        }
        archive_entry_free(e);
    }
    if (archive_write_close(a.get()) != ARCHIVE_OK) {
        throw std::runtime_error("archive: " + archiveError(a.get()));
    }
    return out;
}

std::string PackageWriter::pack(const fs::path& sourceDir, const SecretKey& key) {
    PackageFiles files;
    for (const auto& item : fs::recursive_directory_iterator(sourceDir)) {
        if (item.is_symlink()) throw std::invalid_argument("symlinks are not allowed: " + item.path().string());
        if (!item.is_regular_file()) continue;
        const std::string rel = fs::relative(item.path(), sourceDir).generic_string();
        if (rel.rfind("META/", 0) == 0) continue;
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
    std::optional<TrustedKey> signer;
    try {
        const json sig = json::parse(sigIt->second);
        if (sig.at("alg").get<std::string>() != "ed25519") throw std::invalid_argument("unsupported algorithm");
        const std::string keyId = sig.at("keyId").get<std::string>();
        signer = trust.find(keyId);
        if (!signer) {
            report.fail("signature", "signed by key " + keyId + ", which this server does not trust");
            return std::nullopt;
        }
        if (!signer->key.verify(listIt->second, fromBase64(sig.at("signature").get<std::string>()))) {
            report.fail("signature", "signature does not match the file list");
            return std::nullopt;
        }
        report.pass("signature", "valid ed25519 signature by " + signer->label + " (" + keyId + ")" +
                                     (signer->firstParty() ? ", first-party" : ", third-party: runs isolated"));
    } catch (const std::exception& e) {
        report.fail("signature", std::string("unreadable META/signature: ") + e.what());
        return std::nullopt;
    }

    std::string listError;
    const auto listed = parseFileList(listIt->second, listError);
    if (!listError.empty()) {
        report.fail("files.integrity", listError);
        return std::nullopt;
    }
    for (const auto& [path, content] : files->files) {
        if (path.rfind("META/", 0) == 0) {
            if (path != kFileList && path != kSignature) {
                report.fail("files.integrity", "unexpected file " + path);
                return std::nullopt;
            }
            continue;
        }
        const auto it = listed.find(path);
        if (it == listed.end()) {
            report.fail("files.integrity", path + " is not covered by the signature");
            return std::nullopt;
        }
        if (it->second != Sha256::hex(content)) {
            report.fail("files.integrity", path + " was changed after signing");
            return std::nullopt;
        }
    }
    for (const auto& [path, hash] : listed) {
        if (!files->files.count(path)) {
            report.fail("files.integrity", path + " is listed but missing");
            return std::nullopt;
        }
    }
    report.pass("files.integrity", std::to_string(listed.size()) + " files match their SHA-256");

    const auto manifestIt = files->files.find("manifest.json");
    if (manifestIt == files->files.end()) {
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
    auto manifest = ManifestParser::parse(document, hostVersion, report);
    if (!manifest) return std::nullopt;

    std::vector<std::string> referenced;
    for (const auto& [platform, lib] : manifest->entry) referenced.push_back(lib);
    if (manifest->ui) referenced.push_back(manifest->ui->bundle);
    if (manifest->optionsSchema) referenced.push_back(*manifest->optionsSchema);
    std::string missing;
    for (const std::string& r : referenced) {
        if (!files->files.count(r)) missing += (missing.empty() ? "" : ", ") + r;
    }
    if (!missing.empty()) {
        report.fail("files.referenced", "manifest names files the package lacks: " + missing);
        return std::nullopt;
    }
    if (manifest->optionsSchema) {
        if (!json::accept(files->files.at(*manifest->optionsSchema))) {
            report.fail("files.referenced", *manifest->optionsSchema + " is not JSON");
            return std::nullopt;
        }
    }
    report.pass("files.referenced", "every file named in the manifest is present");

    return VerifiedPackage{std::move(*files), std::move(*manifest), *signer};
}

}  // namespace rfmod
