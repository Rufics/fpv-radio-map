#include "region_archive.hpp"

#include "region_definition.hpp"
#include "security.hpp"

#include <archive_entry.h>

#include <vector>

namespace rfmap {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kChunk = std::size_t{1} << 20;

struct FreeEntry {
    void operator()(archive_entry* e) const { archive_entry_free(e); }
};

/// Reads `file` in chunks and hands each to `sink`, checking `stop` between them.
template <typename Sink>
void readChunks(const fs::path& file, const std::stop_token& stop, std::uintmax_t expected, const Sink& sink) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw RegionError("cannot read " + file.string());
    std::vector<char> buffer(kChunk);
    std::uintmax_t total = 0;
    while (in) {
        if (stop.stop_requested()) throw RegionBuildCancelled();
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        sink(buffer.data(), got);
        total += got;
    }
    if (expected != 0 && total != expected) throw RegionError(file.string() + " changed while it was packaged");
}

}  // namespace

std::string Sha256Stream::hex() {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state_, digest.data());
    return Secrets::hex(digest.data(), digest.size());
}

std::string Sha256Stream::ofFile(const fs::path& file, const std::stop_token& stop) {
    Sha256Stream hash;
    readChunks(file, stop, 0, [&hash](const char* data, std::size_t size) { hash.update(data, size); });
    return hash.hex();
}

ZipWriter::ZipWriter(const fs::path& file) : out_(file, std::ios::binary | std::ios::trunc), archive_(archive_write_new()) {
    if (!out_) throw RegionError("cannot write " + file.string());
    archive* a = archive_.get();
    check(archive_write_set_format_zip(a));
    check(archive_write_set_options(a, "zip:compression=store"));
    check(archive_write_set_bytes_per_block(a, 0));  // no padding after the central directory
    check(archive_write_open(a, this, nullptr, &ZipWriter::write, nullptr));
}

ZipWriter::~ZipWriter() = default;

la_ssize_t ZipWriter::write(archive*, void* self, const void* buffer, std::size_t length) {
    auto* zip = static_cast<ZipWriter*>(self);
    zip->out_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(length));
    if (!zip->out_) return -1;
    zip->hash_.update(buffer, length);
    zip->size_ += length;
    return static_cast<la_ssize_t>(length);
}

void ZipWriter::header(const std::string& name, std::uintmax_t size) {
    const std::unique_ptr<archive_entry, FreeEntry> entry(archive_entry_new());
    archive_entry_set_pathname_utf8(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
    archive_entry_set_mtime(entry.get(), 0, 0);
    check(archive_write_header(archive_.get(), entry.get()));
}

void ZipWriter::data(const void* bytes, std::size_t size) {
    if (archive_write_data(archive_.get(), bytes, size) != static_cast<la_ssize_t>(size)) check(ARCHIVE_FATAL);
}

void ZipWriter::addBytes(const std::string& name, std::string_view bytes) {
    header(name, bytes.size());
    data(bytes.data(), bytes.size());
}

void ZipWriter::addFile(const std::string& name, const fs::path& file, const std::stop_token& stop) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) throw RegionError("cannot read " + file.string());
    header(name, size);
    readChunks(file, stop, size, [this](const char* bytes, std::size_t n) { data(bytes, n); });
}

std::pair<std::uintmax_t, std::string> ZipWriter::close() {
    check(archive_write_close(archive_.get()));
    out_.close();
    if (!out_) throw RegionError("cannot finish the package file");
    return {size_, hash_.hex()};
}

void ZipWriter::check(int status) const {
    if (status >= ARCHIVE_WARN) return;
    const char* message = archive_error_string(archive_.get());
    throw RegionError(std::string("cannot write the package: ") + (message ? message : "unknown error"));
}

}  // namespace rfmap
