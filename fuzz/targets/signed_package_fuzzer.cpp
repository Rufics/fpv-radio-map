// The package upload check (rfmod/package.hpp) past the signature: the file list, file hashes,
// manifest schema and rules, and the files the manifest references. Packages are signed here
// with the public development key, so every input gets as far as a trusted signer's would.
//
// Input: files as text sections, "--- <path>\n<content>" each (no leading "--- ": the whole
// input is manifest.json). A META/files.sha256 section is signed as given instead of the list
// computed from the files; a META/signature section replaces the signature.
// Property: verify() never throws.

#include "../support.hpp"

#include <rfmod/package.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>

namespace {

/// path -> content
using Files = std::map<std::string, std::string, std::less<>>;

const rfmod::SecretKey& devKey() {
    static const rfmod::SecretKey key = [] {
        std::ifstream in(RFMAP_FUZZ_DEV_OWNER_SECRET);
        std::stringstream text;
        text << in.rdbuf();
        return rfmod::SecretKey::fromText(text.str());
    }();
    return key;
}

Files splitSections(std::string_view text) {
    constexpr std::string_view kMark = "--- ";
    Files files;
    if (!text.starts_with(kMark)) {
        files.try_emplace("manifest.json", text);
        return files;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nameStart = pos + kMark.size();
        const std::size_t nameEnd = std::min(text.find('\n', nameStart), text.size());
        const std::size_t bodyStart = std::min(nameEnd + 1, text.size());
        const std::size_t next = text.find("\n--- ", bodyStart);
        const std::size_t bodyEnd = next == std::string_view::npos ? text.size() : next;
        files.try_emplace(std::string(text.substr(nameStart, nameEnd - nameStart)),
                          text.substr(bodyStart, bodyEnd - bodyStart));
        pos = next == std::string_view::npos ? text.size() : next + 1;
    }
    return files;
}

/// Plain tar (no gzip, to keep runs fast; the reader accepts both), written as given.
std::string writeTar(const Files& files) {
    std::unique_ptr<archive, decltype(&archive_write_free)> a(archive_write_new(), archive_write_free);
    archive_write_set_format_pax_restricted(a.get());
    archive_write_set_bytes_in_last_block(a.get(), 1);
    std::string out;
    auto writeCb = [](archive*, void* ctx, const void* buf, std::size_t len) -> la_ssize_t {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(buf), len);
        return static_cast<la_ssize_t>(len);
    };
    archive_write_open(a.get(), &out, nullptr, writeCb, nullptr);
    for (const auto& [path, content] : files) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname(e.get(), path.c_str());
        archive_entry_set_size(e.get(), static_cast<la_int64_t>(content.size()));
        archive_entry_set_filetype(e.get(), AE_IFREG);
        archive_entry_set_perm(e.get(), 0644);
        if (archive_write_header(a.get(), e.get()) != ARCHIVE_OK) continue;
        archive_write_data(a.get(), content.data(), content.size());
    }
    archive_write_close(a.get());
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const rfmod::Version host = rfmod::Version::parse(RFMAP_HOST_VERSION);
    static const rfmod::TrustStore trust(devKey().publicKey().toText(),
                                         rfmap::fuzz::noSuchKeysFile());

    Files files = splitSections(rfmap::fuzz::asText(data, size));
    if (!files.contains("META/files.sha256")) {
        rfmod::PackageFiles content;
        content.files.insert(files.begin(), files.end());
        files["META/files.sha256"] = rfmod::PackageWriter::fileList(content);
    }
    if (!files.contains("META/signature")) {
        const rfmod::Bytes sig = devKey().sign(files.at("META/files.sha256"));
        files["META/signature"] = nlohmann::json{{"alg", "ed25519"},
                                                 {"keyId", devKey().publicKey().id()},
                                                 {"signature", rfmod::toBase64(sig.data(), sig.size())}}
                                      .dump();
    }

    rfmod::ValidationReport report;
    const auto verified = rfmod::PackageVerifier::verify(writeTar(files), trust, host, report);
    rfmap::fuzz::check(verified.has_value() == report.ok());
    return 0;
}
