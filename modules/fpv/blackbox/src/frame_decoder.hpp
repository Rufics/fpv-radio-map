#pragma once

// Decodes the fields of one frame: the encodings of blackbox_encoding.c, then the predictors of
// blackbox.c (the value written is the field minus its prediction). Both are numbered as in
// Betaflight's blackbox_fielddefs.h; every firmware since Cleanflight uses the same numbers.

#include "byte_reader.hpp"
#include "log_header.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fpv::blackbox {

enum Encoding : int {
    kEncSignedVB = 0,
    kEncUnsignedVB = 1,
    kEncNeg14Bit = 3,
    kEncTag8_8SVB = 6,
    kEncTag2_3S32 = 7,
    kEncTag8_4S16 = 8,
    kEncNull = 9,
    kEncTag2_3SVariable = 10,
};

enum Predictor : int {
    kPredZero = 0,
    kPredPrevious = 1,
    kPredStraightLine = 2,
    kPredAverage2 = 3,
    kPredMinThrottle = 4,
    kPredMotor0 = 5,
    kPredIncrement = 6,
    kPredHomeCoord = 7,
    kPred1500 = 8,
    kPredVbatRef = 9,
    kPredLastMainFrameTime = 10,
    kPredMinMotor = 11,
};

/// Reads the raw (encoded) values of a frame. Tagged encodings pack a group of fields; the group
/// takes the fields that follow, as the firmware writes it.
class FieldReader {
public:
    explicit FieldReader(int dataVersion) : dataVersion_(dataVersion) {}

    /// Fills `raw` (one per field). False if an encoding is unknown (see unsupported()) or the
    /// reader went bad.
    bool read(ByteReader& in, const FrameDefinition& def, std::vector<std::int64_t>& raw);
    [[nodiscard]] const std::optional<int>& unsupported() const { return unsupported_; }

    static std::int32_t signExtend(std::uint32_t value, unsigned bits);

private:
    static void readTag2_3S32(ByteReader& in, std::span<std::int64_t, 3> out);
    static void readTag2_3SVariable(ByteReader& in, std::span<std::int64_t, 3> out);
    void readTag8_4S16(ByteReader& in, std::span<std::int64_t, 4> out) const;
    static void readTag8_4S16v1(ByteReader& in, std::uint8_t selector, std::span<std::int64_t, 4> out);
    static void readTag8_4S16v2(ByteReader& in, std::uint8_t selector, std::span<std::int64_t, 4> out);
    static void readTag8_8SVB(ByteReader& in, std::span<std::int64_t> out);
    static void readGroupedBytes(ByteReader& in, std::uint8_t selectors, std::span<std::int64_t, 3> out);
    /// Copies a decoded group into raw[i...], dropping values past the last field.
    static std::size_t store(std::span<const std::int64_t> group, std::vector<std::int64_t>& raw, std::size_t i);

    int dataVersion_;
    std::optional<int> unsupported_;
};

/// Everything outside the frame a predictor may refer to.
struct PredictionContext {
    std::span<const std::int64_t> previous;   ///< empty before the first frame of the stream
    std::span<const std::int64_t> previous2;  ///< empty before the first frame of the stream
    std::uint32_t skippedIterations = 0;
    std::int64_t lastMainFrameTime = 0;
    std::array<std::int64_t, 2> home{0, 0};
};

/// Header constants the predictors use (minthrottle, vbatref, motorOutput).
class Predictors {
public:
    explicit Predictors(const LogHeader& header);

    /// Turns raw values into field values in place, then truncates each to 32 bits as the
    /// firmware's int32/uint32 fields. False if a predictor is unknown (see unsupported()).
    bool apply(const FrameDefinition& def, const PredictionContext& ctx, std::vector<std::int64_t>& values);
    [[nodiscard]] const std::optional<int>& unsupported() const { return unsupported_; }

private:
    std::optional<std::int64_t> predict(const FrameDefinition& def, std::size_t i, const PredictionContext& ctx,
                                        const std::vector<std::int64_t>& values) const;

    std::int64_t minThrottle_ = 0;
    std::int64_t vbatRef_ = 0;
    std::int64_t minMotor_ = 0;
    std::optional<int> unsupported_;
};

}  // namespace fpv::blackbox
