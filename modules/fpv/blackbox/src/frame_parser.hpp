#pragma once

// Walks the frames of one log after its header, the way Betaflight writes them in
// blackbox.c: I (intra) and P (inter) main frames, G (GPS), H (GPS home), S (slow) and E (event).
//
// A frame counts only when the next byte starts another frame (or the data ends) and it is at
// most 256 bytes long. Anything else is corruption: the parser steps one byte past the bad frame
// start, ignores P frames until an I frame resynchronises the main stream, and reports the time
// it lost as a gap. Main frames must not go back in time or iteration, nor jump more than 10 s or
// 5000 iterations, unless a LOGGING_RESUME event announced the jump.

#include "byte_reader.hpp"
#include "frame_decoder.hpp"
#include "log_header.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fpv::blackbox {

/// Event frame types (FLIGHT_LOG_EVENT_*).
inline constexpr int kEventSyncBeep = 0;
inline constexpr int kEventInflightAdjustment = 13;
inline constexpr int kEventLoggingResume = 14;
inline constexpr int kEventDisarm = 15;
inline constexpr int kEventFlightMode = 30;
inline constexpr int kEventLogEnd = 255;

struct LogEvent {
    int type = 0;
    std::int64_t timeUs = 0;       ///< main frame time it follows, or its own time
    std::uint32_t value = 0;       ///< disarm reason, flight mode flags, resumed iteration
    std::uint32_t lastValue = 0;   ///< previous flight mode flags
    int function = 0;              ///< in-flight adjustment function
    double adjustment = 0.0;       ///< in-flight adjustment value
};

enum class GapCause { Corrupt, Missing, Paused };

/// Receives decoded frames in file order. Values are indexed like the header's fields.
class IFrameSink {
public:
    IFrameSink() = default;
    IFrameSink(const IFrameSink&) = delete;
    IFrameSink& operator=(const IFrameSink&) = delete;
    IFrameSink(IFrameSink&&) = delete;
    IFrameSink& operator=(IFrameSink&&) = delete;
    virtual ~IFrameSink() = default;

    virtual void onMainFrame(std::span<const std::int64_t> values, std::int64_t timeUs) = 0;
    virtual void onGpsFrame(std::span<const std::int64_t> values, std::int64_t timeUs) = 0;
    virtual void onHomeFrame(std::span<const std::int64_t> values, std::int64_t timeUs) = 0;
    virtual void onSlowFrame(std::span<const std::int64_t> values, std::int64_t timeUs) = 0;
    virtual void onEvent(const LogEvent& event) = 0;
    virtual void onGap(std::int64_t fromUs, std::int64_t toUs, GapCause cause) = 0;
};

struct ParseStats {
    std::map<char, std::size_t> frames;  ///< accepted frames per marker
    std::size_t corruptFrames = 0;
    bool logEnd = false;
    std::optional<int> unsupportedEncoding;
    std::optional<int> unsupportedPredictor;
    std::optional<std::int64_t> firstTimeUs;
    std::optional<std::int64_t> lastTimeUs;
};

class FrameParser {
public:
    static constexpr std::size_t kMaxFrameBytes = 256;
    static constexpr std::int64_t kMaxTimeJumpUs = 10'000'000;
    static constexpr std::uint32_t kMaxIterationJump = 5000;
    /// Main frames further apart than this, with nothing corrupt in between, are a gap.
    static constexpr std::int64_t kMissingGapUs = 500'000;

    /// `data` is the log after its header, up to the next log in the file.
    FrameParser(const LogHeader& header, std::span<const std::uint8_t> data);

    ParseStats run(IFrameSink& sink);

private:
    struct MainPosition {
        std::uint32_t iteration = 0;
        std::int64_t timeUs = 0;
    };
    enum class Outcome { Accepted, Rejected, Corrupt, Stop };

    static bool isMarker(int byte);
    /// Reads the frame body after its marker; Corrupt when it cannot be read.
    Outcome readFrame(char marker);
    bool frameComplete(std::size_t start, char marker) const;
    void commit(char marker);

    Outcome readMain(char marker);
    Outcome readSimple(char marker, std::vector<std::int64_t>& out);
    Outcome readEvent();
    bool decodeFields(const FrameDefinition& def, const PredictionContext& ctx, std::vector<std::int64_t>& out);
    [[nodiscard]] Outcome decodeOutcome(bool decoded) const;

    void commitMain(char marker);
    void commitIntra(const MainPosition& pos);
    void commitInter(const MainPosition& pos);
    void acceptMain(const MainPosition& pos, bool intra);
    void commitEvent();
    void markCorrupt();

    [[nodiscard]] MainPosition positionOf(const std::vector<std::int64_t>& values) const;
    [[nodiscard]] static bool plausible(const MainPosition& from, const MainPosition& to);
    [[nodiscard]] std::int64_t unwrapTime(std::uint32_t raw) const;

    const LogHeader& header_;
    ByteReader in_;
    FieldReader fields_;
    Predictors predictors_;
    IFrameSink* sink_ = nullptr;
    ParseStats stats_;

    /// The header's frame definitions; null for a frame type the log does not define.
    struct Definitions {
        const FrameDefinition* intra = nullptr;
        const FrameDefinition* inter = nullptr;
        const FrameDefinition* gps = nullptr;
        const FrameDefinition* home = nullptr;
        const FrameDefinition* slow = nullptr;
    };
    /// The main frames before the current one, for the predictors.
    struct History {
        std::vector<std::int64_t> previous;
        std::vector<std::int64_t> previous2;
    };
    /// The main frame clock: wraps of the 32-bit time, pauses, and skipped iterations.
    struct Clock {
        std::int64_t rolloverUs = 0;
        bool resumed = false;
        std::uint32_t skipped = 0;
    };

    Definitions defs_;
    std::size_t iterationField_ = 0;
    std::size_t timeField_ = 1;
    std::optional<std::size_t> gpsTimeField_;

    History history_;
    std::vector<std::int64_t> scratch_;
    std::vector<std::int64_t> raw_;
    std::array<std::int64_t, 2> homeCoord_{0, 0};
    LogEvent event_;

    bool mainValid_ = false;
    std::optional<MainPosition> last_;
    std::optional<MainPosition> candidate_;  ///< an implausible I frame waiting for a second one
    std::optional<std::int64_t> gapFromUs_;  ///< corruption began after this main frame time
    Clock clock_;
};

}  // namespace fpv::blackbox
