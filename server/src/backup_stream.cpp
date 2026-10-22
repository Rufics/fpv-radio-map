#include "backup_stream.hpp"

#include "backup.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <string>

namespace rfmap {

namespace {

/// zstd's level 9 packs JSON Lines and base64 a good deal tighter than its default 3, still at
/// tens of megabytes a second.
constexpr const char* kLevel = "9";

std::string archiveError(archive* a) {
    const char* message = archive_error_string(a);
    return message ? message : "unknown error";
}

void check(archive* a, int status, const char* what) {
    if (status != ARCHIVE_OK) throw BackupError(std::string(what) + ": " + archiveError(a));
}

}  // namespace

void ArchiveFree::operator()(archive* a) const { archive_free(a); }

BackupCompressor::BackupCompressor(std::ostream& out) : out_(out), archive_(archive_write_new()) {
    archive* a = archive_.get();
    // A filter that libarchive would run as an outside zstd program is refused (ARCHIVE_WARN).
    check(a, archive_write_add_filter_zstd(a), "zstd compression");
    check(a, archive_write_set_filter_option(a, "zstd", "compression-level", kLevel), "zstd compression");
    check(a, archive_write_set_format_raw(a), "backup file");
    // Without blocking the output ends with the zstd frame, not with padding up to a block.
    check(a, archive_write_set_bytes_per_block(a, 0), "backup file");
    // libarchive's write callback type fixes the untyped client data and buffer pointers.
    auto write = [](archive*, void* self, const void* bytes, std::size_t n) {  // NOSONAR(cpp:S5008): libarchive API
        std::ostream& sink = static_cast<BackupCompressor*>(self)->out_;
        sink.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(n));
        return sink ? static_cast<la_ssize_t>(n) : la_ssize_t{-1};
    };
    check(a, archive_write_open(a, this, nullptr, write, nullptr), "backup file");
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_pathname(entry.get(), "backup.jsonl");
    check(a, archive_write_header(a, entry.get()), "backup file");
    setp(buffer_.data(), buffer_.data() + buffer_.size());
}

void BackupCompressor::drain() {
    const auto n = static_cast<std::size_t>(pptr() - pbase());
    if (n > 0 && archive_write_data(archive_.get(), pbase(), n) != static_cast<la_ssize_t>(n)) {
        throw BackupError("cannot write the backup: " + archiveError(archive_.get()));
    }
    setp(buffer_.data(), buffer_.data() + buffer_.size());
}

BackupCompressor::int_type BackupCompressor::overflow(int_type ch) {
    drain();
    if (!traits_type::eq_int_type(ch, traits_type::eof())) sputc(traits_type::to_char_type(ch));
    return traits_type::not_eof(ch);
}

int BackupCompressor::sync() {
    drain();
    return 0;
}

void BackupCompressor::finish() {
    drain();
    check(archive_.get(), archive_write_close(archive_.get()), "cannot write the backup");
    if (!out_.flush()) throw BackupError("cannot write the backup");
}

BackupDecompressor::BackupDecompressor(std::istream& in) : in_(in), archive_(archive_read_new()) {
    archive* a = archive_.get();
    // Only the built-in zstd decoder, never an outside program; a plain backup passes through as is.
    check(a, archive_read_support_filter_zstd(a), "zstd decompression");
    check(a, archive_read_support_format_raw(a), "backup file");
    // libarchive's read callback type fixes the untyped client data and buffer pointers.
    auto read = [](archive* ar, void* self, const void** bytes) {  // NOSONAR(cpp:S5008): libarchive API
        auto* d = static_cast<BackupDecompressor*>(self);
        d->in_.read(d->raw_.data(), static_cast<std::streamsize>(d->raw_.size()));
        if (d->in_.bad()) {
            archive_set_error(ar, EIO, "cannot read the backup");
            return la_ssize_t{-1};
        }
        *bytes = d->raw_.data();
        return static_cast<la_ssize_t>(d->in_.gcount());
    };
    check(a, archive_read_open(a, this, nullptr, read, nullptr), "the backup cannot be read");
    archive_entry* entry = nullptr;
    // An empty input has no entry: it reads as an empty backup, which the reader refuses.
    if (const int status = archive_read_next_header(a, &entry); status != ARCHIVE_EOF) {
        check(a, status, "the backup cannot be read");
        hasData_ = true;
    }
    setg(buffer_.data(), buffer_.data(), buffer_.data());
}

BackupDecompressor::int_type BackupDecompressor::underflow() {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (!hasData_) return traits_type::eof();
    const la_ssize_t n = archive_read_data(archive_.get(), buffer_.data(), buffer_.size());
    if (n < 0) throw BackupError("the backup is damaged: " + archiveError(archive_.get()));
    if (n == 0) return traits_type::eof();
    setg(buffer_.data(), buffer_.data(), buffer_.data() + n);
    return traits_type::to_int_type(*gptr());
}

}  // namespace rfmap
