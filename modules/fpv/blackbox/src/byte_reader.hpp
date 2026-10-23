#pragma once

// Reads the primitive values of the Blackbox format, as src/main/blackbox/blackbox_io.c and
// blackbox_encoding.c of Betaflight write them. A read past the end, or a variable-byte number
// longer than five bytes, never throws: it returns 0 and marks the reader bad, so the frame being
// read is dropped and the parser resynchronises.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fpv::blackbox {

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] std::size_t offset() const { return pos_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }
    [[nodiscard]] bool atEnd() const { return pos_ >= data_.size(); }
    /// True once a read ran past the end or met a malformed number since the last seek().
    [[nodiscard]] bool bad() const { return bad_; }

    void seek(std::size_t pos) {
        pos_ = pos < data_.size() ? pos : data_.size();
        bad_ = false;
    }

    /// The next byte without consuming it, or -1 at the end.
    [[nodiscard]] int peek() const { return atEnd() ? -1 : data_[pos_]; }

    std::uint8_t readByte() {
        if (atEnd()) {
            bad_ = true;
            return 0;
        }
        return data_[pos_++];
    }

    /// blackboxWriteUnsignedVB: 7 bits per byte, least significant first, high bit = more follow.
    std::uint32_t readUnsignedVB() {
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            const unsigned b = readByte();
            if (bad_) return 0;
            result |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) return result;
        }
        bad_ = true;  // a 32-bit value never needs a sixth byte
        return 0;
    }

    /// blackboxWriteSignedVB: ZigZag, then unsigned VB.
    std::int32_t readSignedVB() {
        const std::uint32_t u = readUnsignedVB();
        return static_cast<std::int32_t>((u >> 1) ^ (0u - (u & 1u)));
    }

    /// blackboxWriteFloat: the IEEE-754 bits, least significant byte first.
    float readFloat() {
        std::uint32_t bits = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) bits |= static_cast<std::uint32_t>(readByte()) << shift;
        return std::bit_cast<float>(bits);
    }

    /// True if the bytes at the current position equal `text`; nothing is consumed.
    [[nodiscard]] bool lookingAt(std::string_view text) const {
        if (data_.size() - pos_ < text.size()) return false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (data_[pos_ + i] != static_cast<std::uint8_t>(text[i])) return false;
        }
        return true;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool bad_ = false;
};

}  // namespace fpv::blackbox
