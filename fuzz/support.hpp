#pragma once

// Helpers shared by the fuzz targets.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

/// A property every input must keep. Aborts, so libFuzzer saves the input as a crash.
#define RFMAP_FUZZ_CHECK(cond)                                                              \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "%s:%d: fuzz property failed: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                   \
        }                                                                                   \
    } while (false)

namespace rfmap::fuzz {

inline std::string_view asText(const std::uint8_t* data, std::size_t size) {
    return {reinterpret_cast<const char*>(data), size};
}

/// Reads typed values off the front of the input. Past the end every read returns zeros, so any
/// input is a valid one.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool empty() const { return pos_ >= size_; }

    std::uint8_t byte() { return pos_ < size_ ? data_[pos_++] : 0; }

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | byte();
        return v;
    }

    /// A value in [lo, hi], spread evenly over 32 bits of input.
    double range(double lo, double hi) {
        return lo + (hi - lo) * (static_cast<double>(u32()) / 4294967295.0);
    }

    /// Whatever input is left.
    std::string_view rest() {
        const std::size_t from = pos_ < size_ ? pos_ : size_;
        pos_ = size_;
        return {reinterpret_cast<const char*>(data_) + from, size_ - from};
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace rfmap::fuzz
