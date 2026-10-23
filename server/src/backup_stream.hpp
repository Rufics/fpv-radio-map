#pragma once

// The file of a backup on disk: zstd-compressed JSON Lines, through libarchive, which the module
// packages use already. Reading takes a compressed backup and a plain one alike, so a backup
// written before compression still restores, and `zstdcat backup | head -1` shows the header.

#include <array>
#include <cstddef>
#include <istream>
#include <memory>
#include <ostream>
#include <streambuf>

struct archive;

namespace rfmap {

/// The bytes a backup stream buffers at a time.
inline constexpr std::size_t kBackupChunk = std::size_t{64} * 1024;

/// Frees a libarchive handle, for reading or for writing.
struct ArchiveFree {
    void operator()(archive* a) const;
};

/// A stream buffer that compresses what is written to it into `out`. finish() ends the zstd frame
/// and reports any error; output that was not finished is incomplete and does not restore.
class BackupCompressor final : public std::streambuf {
public:
    explicit BackupCompressor(std::ostream& out);
    BackupCompressor(const BackupCompressor&) = delete;
    BackupCompressor& operator=(const BackupCompressor&) = delete;
    ~BackupCompressor() override = default;

    void finish();

protected:
    int_type overflow(int_type ch) override;
    int sync() override;

private:
    /// Hands the buffered bytes to the compressor.
    void drain();

    std::ostream& out_;
    std::unique_ptr<archive, ArchiveFree> archive_;
    std::array<char, kBackupChunk> buffer_{};
};

/// A stream buffer that reads `in` as a backup file: zstd-compressed, or plain. A damaged file
/// throws BackupError from the reading stream (set badbit in its exceptions to see the message).
class BackupDecompressor final : public std::streambuf {
public:
    explicit BackupDecompressor(std::istream& in);
    BackupDecompressor(const BackupDecompressor&) = delete;
    BackupDecompressor& operator=(const BackupDecompressor&) = delete;
    ~BackupDecompressor() override = default;

protected:
    int_type underflow() override;

private:
    std::istream& in_;
    std::unique_ptr<archive, ArchiveFree> archive_;
    /// False for an empty input, which has no data to read.
    bool hasData_ = false;
    /// What libarchive reads from `in_`, and what it hands back decompressed.
    std::array<char, kBackupChunk> raw_{};
    std::array<char, kBackupChunk> buffer_{};
};

}  // namespace rfmap
