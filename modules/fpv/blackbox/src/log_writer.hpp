#pragma once

// Writes Blackbox data the way blackbox_encoding.c does, for the module's self-test, the unit
// tests and the fuzz seeds. Only the encoder: the caller works out the predicted deltas.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fpv::blackbox {

class LogWriter {
public:
    void headerLine(std::string_view name, std::string_view value) {
        text("H ");
        text(name);
        byte(':');
        text(value);
        byte('\n');
    }
    void byte(std::uint32_t b) { out_.push_back(static_cast<std::uint8_t>(b & 0xFFu)); }
    void text(std::string_view s) {
        for (const char c : s) byte(static_cast<std::uint8_t>(c));
    }

    void unsignedVB(std::uint32_t v) {
        while (v > 127) {
            byte((v & 0x7Fu) | 0x80u);
            v >>= 7;
        }
        byte(v);
    }
    void signedVB(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        unsignedVB((u << 1) ^ (0u - (u >> 31)));
    }

    /// blackboxWriteTag8_8SVB for `values.size()` (1..8) fields.
    void tag8_8SVB(std::span<const std::int32_t> values) {
        if (values.size() == 1) {
            signedVB(values[0]);
            return;
        }
        std::uint32_t header = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0) header |= 1u << i;
        }
        byte(header);
        for (const std::int32_t v : values) {
            if (v != 0) signedVB(v);
        }
    }

    /// blackboxWriteTag2_3S32.
    void tag2_3S32(std::span<const std::int32_t, 3> v) {
        int selector = 0;
        for (const std::int32_t x : v) selector = std::max(selector, bitsClass(x));
        switch (selector) {
        case 0:
            byte(static_cast<std::uint32_t>((v[0] & 3) << 4 | (v[1] & 3) << 2 | (v[2] & 3)));
            break;
        case 1:
            byte(0x40u | static_cast<std::uint32_t>(v[0] & 0x0F));
            byte(static_cast<std::uint32_t>((v[1] & 0x0F) << 4 | (v[2] & 0x0F)));
            break;
        case 2:
            byte(0x80u | static_cast<std::uint32_t>(v[0] & 0x3F));
            byte(static_cast<std::uint32_t>(v[1] & 0x3F));
            byte(static_cast<std::uint32_t>(v[2] & 0x3F));
            break;
        default: {
            std::uint32_t sel = 0;
            for (int i = 2; i >= 0; --i) sel = (sel << 2) | byteClass(v[static_cast<std::size_t>(i)]);
            byte(0xC0u | sel);
            for (std::size_t i = 0; i < 3; ++i, sel >>= 2) {
                for (std::uint32_t b = 0; b <= (sel & 3u); ++b) byte(static_cast<std::uint32_t>(v[i]) >> (8 * b));
            }
            break;
        }
        }
    }

    /// blackboxWriteTag8_4S16 (data version 2).
    void tag8_4S16(std::span<const std::int32_t, 4> v) {
        std::uint32_t selector = 0;
        for (int i = 3; i >= 0; --i) {
            const std::int32_t x = v[static_cast<std::size_t>(i)];
            std::uint32_t c = 3;
            if (x == 0) {
                c = 0;
            } else if (x >= -8 && x < 8) {
                c = 1;
            } else if (x >= -128 && x < 128) {
                c = 2;
            }
            selector = (selector << 2) | c;
        }
        byte(selector);
        std::vector<std::uint32_t> nibbles;
        for (std::size_t i = 0; i < 4; ++i, selector >>= 2) {
            const auto x = static_cast<std::uint32_t>(v[i]);
            const std::uint32_t count = (selector & 3u) == 0 ? 0 : 1u << ((selector & 3u) - 1);  // 0, 1, 2, 4 nibbles
            for (std::uint32_t n = count; n > 0; --n) nibbles.push_back((x >> (4 * (n - 1))) & 0x0Fu);
        }
        for (std::size_t i = 0; i < nibbles.size(); i += 2) {
            byte(nibbles[i] << 4 | (i + 1 < nibbles.size() ? nibbles[i + 1] : 0u));
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return out_; }

private:
    static int bitsClass(std::int32_t x) {
        if (x >= 32 || x < -32) return 3;
        if (x >= 8 || x < -8) return 2;
        if (x >= 2 || x < -2) return 1;
        return 0;
    }
    static std::uint32_t byteClass(std::int32_t x) {
        if (x >= -128 && x < 128) return 0;
        if (x >= -32768 && x < 32768) return 1;
        if (x >= -8388608 && x < 8388608) return 2;
        return 3;
    }

    std::vector<std::uint8_t> out_;
};

}  // namespace fpv::blackbox
