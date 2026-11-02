#pragma once

// The file-level pieces of a region package: streaming SHA-256 and the zip it is written into.

#include <archive.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sodium.h>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace rfmap {

/// SHA-256 over data handed in pieces.
class Sha256Stream {
public:
    Sha256Stream() { crypto_hash_sha256_init(&state_); }
    void update(const char* data, std::size_t size) {
        crypto_hash_sha256_update(&state_, reinterpret_cast<const unsigned char*>(data), size);
    }
    /// The digest in hex; the stream is finished after it.
    [[nodiscard]] std::string hex();

    /// The SHA-256 of a file, read in chunks with `stop` checked between them.
    [[nodiscard]] static std::string ofFile(const std::filesystem::path& file, const std::stop_token& stop);

private:
    crypto_hash_sha256_state state_{};
};

/// A zip whose entries are stored, written the same way every time: no times, no owners, so the
/// same entries in the same order are the same bytes. Entries over 4 GiB use Zip64.
class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& file);
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void addBytes(const std::string& name, std::string_view bytes);
    /// Adds `file`, read in chunks with `stop` checked between them (RegionBuildCancelled).
    void addFile(const std::string& name, const std::filesystem::path& file, const std::stop_token& stop);
    /// Finishes the zip; returns its size and SHA-256.
    std::pair<std::uintmax_t, std::string> close();

private:
    struct Free {
        void operator()(archive* a) const { archive_write_free(a); }
    };
    static la_ssize_t write(archive*, void* self, const void* buffer, std::size_t length);  // NOSONAR(cpp:S5008): libarchive API
    void header(const std::string& name, std::uintmax_t size) const;
    void data(const char* bytes, std::size_t size) const;
    void check(int status) const;

    std::ofstream out_;
    Sha256Stream hash_;
    std::uintmax_t size_ = 0;
    std::unique_ptr<archive, Free> archive_{archive_write_new()};
};

}  // namespace rfmap
