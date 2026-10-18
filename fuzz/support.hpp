#pragma once

// Helpers shared by the fuzz targets.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <span>
#include <string>

namespace rfmap::fuzz {

/// A property every input must keep. Aborts, so libFuzzer saves the input as a crash.
inline void check(bool holds, const std::source_location where = std::source_location::current()) {
    if (holds) return;
    std::cerr << where.file_name() << ':' << where.line() << ": fuzz property failed\n";
    std::abort();
}

/// The input as text, byte for byte.
inline std::string asText(const std::uint8_t* data, std::size_t size) { return {data, data + size}; }

/// A file in the build directory that never exists, for a trust store with only the built-in key.
inline std::filesystem::path noSuchKeysFile() {
    return std::filesystem::path(RFMAP_FUZZ_BINARY_DIR) / "no-such-keys.json";
}

/// Reads typed values off the front of the input. Past the end every read returns zeros, so any
/// input is a valid one.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : bytes_(std::as_bytes(std::span(data, size))) {}

    [[nodiscard]] bool empty() const { return pos_ >= bytes_.size(); }

    /// One bit of input.
    bool flag() { return next() % 2 == 1; }

    /// A whole number in [0, n), n up to 256.
    unsigned below(unsigned n) { return next() % n; }

    /// A value in [lo, hi], spread evenly over 32 bits of input.
    double range(double lo, double hi) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = v * 256 + next();
        return lo + (hi - lo) * (static_cast<double>(v) / 4294967295.0);
    }

private:
    unsigned next() { return pos_ < bytes_.size() ? std::to_integer<unsigned>(bytes_[pos_++]) : 0U; }

    std::span<const std::byte> bytes_;
    std::size_t pos_ = 0;
};

}  // namespace rfmap::fuzz
