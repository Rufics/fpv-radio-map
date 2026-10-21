#include "frame_decoder.hpp"

#include <algorithm>

namespace fpv::blackbox {
namespace {

constexpr std::size_t kTag8_8Group = 8;

std::uint32_t littleEndian(ByteReader& in, unsigned bytes) {
    std::uint32_t value = 0;
    for (unsigned b = 0; b < bytes; ++b) value |= static_cast<std::uint32_t>(in.readByte()) << (8 * b);
    return value;
}

}  // namespace

std::int32_t FieldReader::signExtend(std::uint32_t value, unsigned bits) {
    if (bits == 0) return 0;
    const std::uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
    value &= mask;
    const std::uint32_t sign = 1u << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

std::size_t FieldReader::store(std::span<const std::int64_t> group, std::vector<std::int64_t>& raw, std::size_t i) {
    for (std::size_t j = 0; j < group.size() && i + j < raw.size(); ++j) raw[i + j] = group[j];
    return group.size();
}

bool FieldReader::read(ByteReader& in, const FrameDefinition& def, std::vector<std::int64_t>& raw) {
    raw.assign(def.size(), 0);
    std::size_t i = 0;
    while (i < def.size() && !in.bad()) {
        const int encoding = def.field(i).encoding;
        switch (encoding) {
        case kEncSignedVB:
            raw[i++] = in.readSignedVB();
            break;
        case kEncUnsignedVB:
            raw[i++] = in.readUnsignedVB();
            break;
        case kEncNeg14Bit:
            raw[i++] = -static_cast<std::int64_t>(signExtend(in.readUnsignedVB(), 14));
            break;
        case kEncNull:
            raw[i++] = 0;
            break;
        case kEncTag8_8SVB: {
            // The group is the run of consecutive TAG8_8SVB fields, at most eight.
            std::size_t n = 1;
            while (n < kTag8_8Group && i + n < def.size() && def.field(i + n).encoding == kEncTag8_8SVB) ++n;
            std::array<std::int64_t, kTag8_8Group> group{};
            readTag8_8SVB(in, std::span<std::int64_t>(group.data(), n));
            i += store(std::span<const std::int64_t>(group.data(), n), raw, i);
            break;
        }
        case kEncTag2_3S32: {
            std::array<std::int64_t, 3> group{};
            readTag2_3S32(in, group);
            i += store(group, raw, i);
            break;
        }
        case kEncTag2_3SVariable: {
            std::array<std::int64_t, 3> group{};
            readTag2_3SVariable(in, group);
            i += store(group, raw, i);
            break;
        }
        case kEncTag8_4S16: {
            std::array<std::int64_t, 4> group{};
            readTag8_4S16(in, group);
            i += store(group, raw, i);
            break;
        }
        default:
            unsupported_ = encoding;
            return false;
        }
    }
    return !in.bad();
}

void FieldReader::readTag8_8SVB(ByteReader& in, std::span<std::int64_t> out) {
    // blackboxWriteTag8_8SVB: one value alone is a plain signed VB; otherwise a byte with a bit
    // per non-zero value (first value in the low bit), then those values.
    if (out.size() == 1) {
        out[0] = in.readSignedVB();
        return;
    }
    const unsigned header = in.readByte();
    for (std::size_t j = 0; j < out.size(); ++j) out[j] = (header >> j) & 1u ? in.readSignedVB() : 0;
}

void FieldReader::readGroupedBytes(ByteReader& in, unsigned selectors, std::span<std::int64_t, 3> out) {
    // Two bits per field, first field in the low bits: 1, 2, 3 or 4 little-endian bytes.
    for (std::size_t j = 0; j < 3; ++j, selectors >>= 2) {
        const unsigned bytes = (selectors & 3u) + 1u;
        out[j] = signExtend(littleEndian(in, bytes), 8 * bytes);
    }
}

void FieldReader::readTag2_3S32(ByteReader& in, std::span<std::int64_t, 3> out) {
    // blackboxWriteTag2_3S32. Top two bits select 2, 4, 6 or 8..32 bits per field.
    const unsigned lead = in.readByte();
    switch (lead >> 6) {
    case 0:
        out[0] = signExtend(lead >> 4, 2);
        out[1] = signExtend(lead >> 2, 2);
        out[2] = signExtend(lead, 2);
        break;
    case 1: {
        out[0] = signExtend(lead, 4);
        const unsigned b = in.readByte();
        out[1] = signExtend(b >> 4, 4);
        out[2] = signExtend(b, 4);
        break;
    }
    case 2:
        out[0] = signExtend(lead, 6);
        out[1] = signExtend(in.readByte(), 6);
        out[2] = signExtend(in.readByte(), 6);
        break;
    default:
        readGroupedBytes(in, lead, out);
        break;
    }
}

void FieldReader::readTag2_3SVariable(ByteReader& in, std::span<std::int64_t, 3> out) {
    // blackboxWriteTag2_3SVariable: 2-2-2, 5-5-4 or 8-7-7 bits, or 8..32 bits per field.
    const unsigned lead = in.readByte();
    switch (lead >> 6) {
    case 0:
        out[0] = signExtend(lead >> 4, 2);
        out[1] = signExtend(lead >> 2, 2);
        out[2] = signExtend(lead, 2);
        break;
    case 1: {
        // ss11 1112 2222 3333
        const unsigned b = in.readByte();
        out[0] = signExtend(lead >> 1, 5);
        out[1] = signExtend(static_cast<std::uint32_t>((lead & 1u) << 4) | (b >> 4), 5);
        out[2] = signExtend(b, 4);
        break;
    }
    case 2: {
        // ss11 1111 1122 2222 2333 3333
        const unsigned b1 = in.readByte();
        const unsigned b2 = in.readByte();
        out[0] = signExtend(static_cast<std::uint32_t>((lead & 0x3Fu) << 2) | (b1 >> 6), 8);
        out[1] = signExtend(static_cast<std::uint32_t>((b1 & 0x3Fu) << 1) | (b2 >> 7), 7);
        out[2] = signExtend(b2, 7);
        break;
    }
    default:
        readGroupedBytes(in, lead, out);
        break;
    }
}

void FieldReader::readTag8_4S16(ByteReader& in, std::span<std::int64_t, 4> out) const {
    const unsigned selector = in.readByte();
    if (dataVersion_ < 2) {
        readTag8_4S16v1(in, selector, out);
    } else {
        readTag8_4S16v2(in, selector, out);
    }
}

void FieldReader::readTag8_4S16v1(ByteReader& in, unsigned selector, std::span<std::int64_t, 4> out) {
    // Data version 1: a 4-bit field shares its byte with the next field (low nibble first),
    // 8-bit is one byte, 16-bit is little-endian.
    std::size_t j = 0;
    while (j < 4) {
        const unsigned width = selector & 3u;
        selector >>= 2;
        switch (width) {
        case 0:
            out[j++] = 0;
            break;
        case 1: {
            const unsigned b = in.readByte();
            out[j++] = signExtend(b, 4);
            if (j < 4) {
                out[j++] = signExtend(b >> 4, 4);
                selector >>= 2;
            }
            break;
        }
        case 2:
            out[j++] = signExtend(in.readByte(), 8);
            break;
        default:
            out[j++] = signExtend(littleEndian(in, 2), 16);
            break;
        }
    }
}

void FieldReader::readTag8_4S16v2(ByteReader& in, unsigned selector, std::span<std::int64_t, 4> out) {
    // blackboxWriteTag8_4S16: values are packed in nibbles, high nibble first; a value that
    // starts in the middle of a byte continues in the next one.
    bool halfByte = false;  // the low nibble of `buffer` is still unread
    unsigned buffer = 0;
    const auto nibble = [&in, &halfByte, &buffer]() -> std::uint32_t {
        if (halfByte) {
            halfByte = false;
            return buffer & 0x0Fu;
        }
        buffer = in.readByte();
        halfByte = true;
        return buffer >> 4;
    };
    for (std::size_t j = 0; j < 4; ++j, selector >>= 2) {
        switch (selector & 3u) {
        case 0:
            out[j] = 0;
            break;
        case 1:
            out[j] = signExtend(nibble(), 4);
            break;
        case 2: {
            const std::uint32_t high = nibble();
            out[j] = signExtend((high << 4) | nibble(), 8);
            break;
        }
        default: {
            std::uint32_t v = 0;
            for (int n = 0; n < 4; ++n) v = (v << 4) | nibble();
            out[j] = signExtend(v, 16);
            break;
        }
        }
    }
}

// blackbox-tools and the firmware default: minthrottle 1150, vbatref 4095.
Predictors::Predictors(const LogHeader& header)
    : minThrottle_(header.integer("minthrottle").value_or(1150)), vbatRef_(header.integer("vbatref").value_or(4095)) {
    const auto motorOutput = header.integers("motorOutput");
    minMotor_ = motorOutput.empty() ? minThrottle_ : motorOutput.front();
}

std::int64_t Predictors::average2(std::int64_t a, std::int64_t b) {
    // The firmware's C division truncates toward zero; std::midpoint rounds toward `a` instead.
    const std::int64_t sum = a + b;
    return sum / 2;
}

std::optional<std::int64_t> Predictors::predict(const FrameDefinition& def, std::size_t i, const PredictionContext& ctx,
                                                const std::vector<std::int64_t>& values) const {
    const bool hasPrevious = i < ctx.previous.size();
    const std::int64_t prev = hasPrevious ? ctx.previous[i] : 0;
    const std::int64_t prev2 = i < ctx.previous2.size() ? ctx.previous2[i] : prev;
    switch (def.field(i).predictor) {
    case kPredZero:
        return 0;
    case kPredPrevious:
        return prev;
    case kPredStraightLine:
        return hasPrevious ? 2 * prev - prev2 : 0;
    case kPredAverage2:
        return hasPrevious ? average2(prev, prev2) : 0;
    case kPredMinThrottle:
        return minThrottle_;
    case kPredMotor0: {
        const auto motor0 = def.indexOf("motor[0]");
        return motor0.has_value() && *motor0 < i ? values[*motor0] : 0;
    }
    case kPredIncrement:
        return prev + ctx.skippedIterations + 1;
    case kPredHomeCoord:
        // GPS_coord[0] (latitude) is relative to GPS_home[0], GPS_coord[1] to GPS_home[1].
        return def.field(i).name.ends_with("[1]") ? ctx.home[1] : ctx.home[0];
    case kPred1500:
        return 1500;
    case kPredVbatRef:
        return vbatRef_;
    case kPredLastMainFrameTime:
        return ctx.lastMainFrameTime;
    case kPredMinMotor:
        return minMotor_;
    default:
        return std::nullopt;
    }
}

bool Predictors::apply(const FrameDefinition& def, const PredictionContext& ctx, std::vector<std::int64_t>& values) {
    for (std::size_t i = 0; i < def.size() && i < values.size(); ++i) {
        const auto prediction = predict(def, i, ctx, values);
        if (!prediction.has_value()) {
            unsupported_ = def.field(i).predictor;
            return false;
        }
        const std::int64_t v = values[i] + *prediction;
        values[i] = def.field(i).isSigned ? static_cast<std::int64_t>(static_cast<std::int32_t>(v))
                                          : static_cast<std::int64_t>(static_cast<std::uint32_t>(v));
    }
    return true;
}

}  // namespace fpv::blackbox
