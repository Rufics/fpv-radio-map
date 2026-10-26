#include "frame_parser.hpp"

#include <algorithm>
#include <string_view>

namespace fpv::blackbox {
namespace {

constexpr std::int64_t kTimeWrap = std::int64_t{1} << 32;
constexpr std::string_view kEndOfLog{"End of log\0", 11};

}  // namespace

FrameParser::FrameParser(const LogHeader& header, std::span<const std::uint8_t> data)
    : header_(header),
      in_(data),
      fields_(header.dataVersion()),
      predictors_(header),
      defs_{header.frame('I'), header.frame('P'), header.frame('G'), header.frame('H'), header.frame('S')} {
    if (defs_.intra) {
        iterationField_ = defs_.intra->indexOf("loopIteration").value_or(0);
        timeField_ = defs_.intra->indexOf("time").value_or(1);
    }
    if (defs_.gps) gpsTimeField_ = defs_.gps->indexOf("time");
}

bool FrameParser::isMarker(int byte) {
    return byte == 'I' || byte == 'P' || byte == 'G' || byte == 'H' || byte == 'S' || byte == 'E';
}

ParseStats FrameParser::run(IFrameSink& sink) {
    using enum Outcome;
    sink_ = &sink;
    bool inCorruptRun = false;
    while (!in_.atEnd() && !stats_.logEnd) {
        const std::size_t start = in_.offset();
        const int marker = in_.readByte();
        Outcome outcome = isMarker(marker) ? readFrame(static_cast<char>(marker)) : Corrupt;
        if (outcome == Stop) break;
        if (outcome == Accepted && !frameComplete(start, static_cast<char>(marker))) outcome = Corrupt;
        if (outcome == Corrupt) {
            // One count per run of bad bytes; the scan restarts one byte after the bad frame start.
            if (!inCorruptRun) ++stats_.corruptFrames;
            inCorruptRun = true;
            markCorrupt();
            in_.seek(start + 1);
            continue;
        }
        inCorruptRun = false;
        commit(static_cast<char>(marker));
    }
    return stats_;
}

bool FrameParser::frameComplete(std::size_t start, char marker) const {
    if (in_.bad() || in_.offset() - start > kMaxFrameBytes) return false;
    // The end marker may be followed by anything (padding, the next log's header).
    if (marker == 'E' && event_.type == kEventLogEnd) return true;
    return in_.atEnd() || isMarker(in_.peek());
}

FrameParser::Outcome FrameParser::readFrame(char marker) {
    using enum Outcome;
    switch (marker) {
    case 'I':
    case 'P':
        return readMain(marker);
    case 'G':
        return defs_.gps ? readSimple(marker, scratch_) : Corrupt;
    case 'H':
        return defs_.home ? readSimple(marker, scratch_) : Corrupt;
    case 'S':
        return defs_.slow ? readSimple(marker, scratch_) : Corrupt;
    default:
        return readEvent();
    }
}

FrameParser::Outcome FrameParser::readMain(char marker) {
    const FrameDefinition* def = marker == 'I' ? defs_.intra : defs_.inter;
    if (def == nullptr || !def->complete()) return Outcome::Corrupt;
    PredictionContext ctx;
    ctx.previous = history_.previous;
    if (marker == 'P') {
        ctx.previous2 = history_.previous2;
        clock_.skipped = last_.has_value() ? header_.schedule().skippedAfter(last_->iteration) : 0;
        ctx.skippedIterations = clock_.skipped;
    }
    return decodeOutcome(decodeFields(*def, ctx, scratch_));
}

FrameParser::Outcome FrameParser::decodeOutcome(bool decoded) const {
    using enum Outcome;
    if (decoded) return Accepted;
    return stats_.unsupportedEncoding.has_value() || stats_.unsupportedPredictor.has_value() ? Stop : Corrupt;
}

FrameParser::Outcome FrameParser::readSimple(char marker, std::vector<std::int64_t>& out) {
    const FrameDefinition* def = defs_.slow;
    if (marker == 'G') def = defs_.gps;
    if (marker == 'H') def = defs_.home;
    if (!def->complete()) return Outcome::Corrupt;
    PredictionContext ctx;
    ctx.home = homeCoord_;
    ctx.lastMainFrameTime = last_.has_value() ? last_->timeUs : 0;
    return decodeOutcome(decodeFields(*def, ctx, out));
}

bool FrameParser::decodeFields(const FrameDefinition& def, const PredictionContext& ctx, std::vector<std::int64_t>& out) {
    if (!fields_.read(in_, def, raw_)) {
        if (fields_.unsupported().has_value()) stats_.unsupportedEncoding = fields_.unsupported();
        return false;
    }
    out = raw_;
    if (!predictors_.apply(def, ctx, out)) {
        stats_.unsupportedPredictor = predictors_.unsupported();
        return false;
    }
    return true;
}

FrameParser::Outcome FrameParser::readEvent() {
    using enum Outcome;
    event_ = LogEvent{};
    event_.type = in_.readByte();
    switch (event_.type) {
    case kEventSyncBeep:
    case kEventDisarm:
        event_.value = in_.readUnsignedVB();
        break;
    case kEventInflightAdjustment: {
        // Bit 7 of the function byte flags a float value, else a signed VB follows.
        const unsigned function = in_.readByte();
        event_.function = function & 0x7F;
        event_.adjustment = (function & 0x80u) != 0 ? static_cast<double>(in_.readFloat())
                                                    : static_cast<double>(in_.readSignedVB());
        break;
    }
    case kEventLoggingResume:  // lastValue: the time logging resumed at
    case kEventFlightMode:     // lastValue: the flags before
        event_.value = in_.readUnsignedVB();
        event_.lastValue = in_.readUnsignedVB();
        break;
    case kEventLogEnd:
        for (const char c : kEndOfLog) {
            if (in_.readByte() != static_cast<std::uint8_t>(c)) return Corrupt;
        }
        break;
    default:
        return Corrupt;
    }
    return in_.bad() ? Corrupt : Accepted;
}

void FrameParser::commit(char marker) {
    switch (marker) {
    case 'I':
    case 'P':
        commitMain(marker);
        break;
    case 'G': {
        std::int64_t t = last_.has_value() ? last_->timeUs : 0;
        if (gpsTimeField_.has_value() && *gpsTimeField_ < scratch_.size()) {
            t = unwrapTime(static_cast<std::uint32_t>(scratch_[*gpsTimeField_]));
        }
        ++stats_.frames['G'];
        sink_->onGpsFrame(scratch_, t);
        break;
    }
    case 'H':
        if (scratch_.size() >= 2) homeCoord_ = {scratch_[0], scratch_[1]};
        ++stats_.frames['H'];
        sink_->onHomeFrame(scratch_, last_.has_value() ? last_->timeUs : 0);
        break;
    case 'S':
        ++stats_.frames['S'];
        sink_->onSlowFrame(scratch_, last_.has_value() ? last_->timeUs : 0);
        break;
    default:
        commitEvent();
        break;
    }
}

void FrameParser::commitMain(char marker) {
    const MainPosition pos = positionOf(scratch_);
    if (marker == 'I') {
        commitIntra(pos);
    } else {
        commitInter(pos);
    }
}

void FrameParser::commitIntra(const MainPosition& pos) {
    if (!last_.has_value() || plausible(*last_, pos) || (candidate_.has_value() && plausible(*candidate_, pos))) {
        acceptMain(pos, true);
        return;
    }
    // Too far from the last good frame: wait for a second I frame that agrees with this one.
    candidate_ = pos;
    if (mainValid_ && last_.has_value()) gapFromUs_ = last_->timeUs;
    mainValid_ = false;
}

void FrameParser::commitInter(const MainPosition& pos) {
    if (!mainValid_ || !last_.has_value()) return;  // a P frame cannot resynchronise the stream
    if (!plausible(*last_, pos)) {
        gapFromUs_ = last_->timeUs;
        mainValid_ = false;
        return;
    }
    acceptMain(pos, false);
}

void FrameParser::acceptMain(const MainPosition& pos, bool intra) {
    if (last_.has_value() && gapFromUs_.has_value()) {
        sink_->onGap(*gapFromUs_, pos.timeUs, GapCause::Corrupt);
    } else if (last_.has_value() && !clock_.resumed && pos.timeUs - last_->timeUs > kMissingGapUs) {
        sink_->onGap(last_->timeUs, pos.timeUs, GapCause::Missing);
    }
    gapFromUs_.reset();
    candidate_.reset();
    clock_.resumed = false;
    mainValid_ = true;
    while (pos.timeUs >= clock_.rolloverUs + kTimeWrap) clock_.rolloverUs += kTimeWrap;

    if (intra) {
        // After an I frame both histories are the I frame: nothing older can be trusted.
        history_.previous = scratch_;
        history_.previous2 = scratch_;
    } else {
        history_.previous2 = std::move(history_.previous);
        history_.previous = scratch_;
    }
    last_ = pos;
    ++stats_.frames[intra ? 'I' : 'P'];
    if (!stats_.firstTimeUs.has_value()) stats_.firstTimeUs = pos.timeUs;
    stats_.lastTimeUs = pos.timeUs;
    sink_->onMainFrame(history_.previous, pos.timeUs);
}

void FrameParser::commitEvent() {
    event_.timeUs = last_.has_value() ? last_->timeUs : 0;
    switch (event_.type) {
    case kEventSyncBeep:
        event_.timeUs = unwrapTime(event_.value);
        break;
    case kEventLoggingResume: {
        // Logging was switched off (BOXBLACKBOX) and on again: frames continue from here.
        const MainPosition resumed{event_.value, unwrapTime(event_.lastValue)};
        if (last_.has_value()) sink_->onGap(last_->timeUs, resumed.timeUs, GapCause::Paused);
        event_.timeUs = resumed.timeUs;
        last_ = resumed;
        clock_.resumed = true;
        mainValid_ = false;
        gapFromUs_.reset();
        break;
    }
    case kEventLogEnd:
        stats_.logEnd = true;
        break;
    default:
        break;
    }
    ++stats_.frames['E'];
    sink_->onEvent(event_);
}

void FrameParser::markCorrupt() {
    if (mainValid_ && last_.has_value()) gapFromUs_ = last_->timeUs;
    mainValid_ = false;
}

FrameParser::MainPosition FrameParser::positionOf(const std::vector<std::int64_t>& values) const {
    MainPosition pos;
    if (iterationField_ < values.size()) pos.iteration = static_cast<std::uint32_t>(values[iterationField_]);
    if (timeField_ < values.size()) pos.timeUs = unwrapTime(static_cast<std::uint32_t>(values[timeField_]));
    return pos;
}

bool FrameParser::plausible(const MainPosition& from, const MainPosition& to) {
    return to.iteration >= from.iteration && to.iteration - from.iteration < kMaxIterationJump && to.timeUs >= from.timeUs &&
           to.timeUs - from.timeUs < kMaxTimeJumpUs;
}

std::int64_t FrameParser::unwrapTime(std::uint32_t raw) const {
    // time is a uint32 of microseconds: it wraps after 71.6 minutes. A small step past the wrap
    // is a rollover, as in blackbox-tools' flightLogApplyMainFrameTimeRollover.
    std::int64_t t = clock_.rolloverUs + raw;
    if (last_.has_value() && t < last_->timeUs) {
        const std::uint32_t forward = raw - static_cast<std::uint32_t>(last_->timeUs);
        if (forward < kMaxTimeJumpUs) t += kTimeWrap;
    }
    return t;
}

}  // namespace fpv::blackbox
