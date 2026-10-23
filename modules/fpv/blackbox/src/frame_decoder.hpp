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

/// Field encodings by the number the log header gives them (FLIGHT_LOG_FIELD_ENCODING_*).
inline constexpr int kEncSignedVB = 0;
inline constexpr int kEncUnsignedVB = 1;
inline constexpr int kEncNeg14Bit = 3;
inline constexpr int kEncTag8_8SVB = 6;
inline constexpr int kEncTag2_3S32 = 7;
inline constexpr int kEncTag8_4S16 = 8;
inline constexpr int kEncNull = 9;
inline constexpr int kEncTag2_3SVariable = 10;

/// Field predictors by the number the log header gives them (FLIGHT_LOG_FIELD_PREDICTOR_*).
inline constexpr int kPredZero = 0;
inline constexpr int kPredPrevious = 1;
inline constexpr int kPredStraightLine = 2;
inline constexpr int kPredAverage2 = 3;
inline constexpr int kPredMinThrottle = 4;
inline constexpr int kPredMotor0 = 5;
inline constexpr int kPredIncrement = 6;
inline constexpr int kPredHomeCoord = 7;
inline constexpr int kPred1500 = 8;
inline constexpr int kPredVbatRef = 9;
inline constexpr int kPredLastMainFrameTime = 10;
inline constexpr int kPredMinMotor = 11;

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
    static void readTag8_4S16v1(ByteReader& in, unsigned selector, std::span<std::int64_t, 4> out);
    static void readTag8_4S16v2(ByteReader& in, unsigned selector, std::span<std::int64_t, 4> out);
    static void readTag8_8SVB(ByteReader& in, std::span<std::int64_t> out);
    static void readGroupedBytes(ByteReader& in, unsigned selectors, std::span<std::int64_t, 3> out);
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
    [[nodiscard]] static std::int64_t average2(std::int64_t a, std::int64_t b);

    std::int64_t minThrottle_ = 0;
    std::int64_t vbatRef_ = 0;
    std::int64_t minMotor_ = 0;
    std::optional<int> unsupported_;
};

}  // namespace fpv::blackbox
