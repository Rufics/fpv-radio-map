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
      intra_(header.frame('I')),
      inter_(header.frame('P')),
      gps_(header.frame('G')),
      home_(header.frame('H')),
      slow_(header.frame('S')) {
    if (intra_) {
        iterationField_ = intra_->indexOf("loopIteration").value_or(0);
        timeField_ = intra_->indexOf("time").value_or(1);
    }
    if (gps_) gpsTimeField_ = gps_->indexOf("time");
}

bool FrameParser::isMarker(int byte) {
    return byte == 'I' || byte == 'P' || byte == 'G' || byte == 'H' || byte == 'S' || byte == 'E';
}

ParseStats FrameParser::run(IFrameSink& sink) {
    sink_ = &sink;
    bool inCorruptRun = false;
    while (!in_.atEnd() && !stats_.logEnd) {
        const std::size_t start = in_.offset();
        const int marker = in_.readByte();
        Outcome outcome = isMarker(marker) ? readFrame(static_cast<char>(marker)) : Outcome::Corrupt;
        if (outcome == Outcome::Stop) break;
        if (outcome == Outcome::Accepted && !frameComplete(start, static_cast<char>(marker))) outcome = Outcome::Corrupt;
        if (outcome == Outcome::Corrupt) {
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
    switch (marker) {
    case 'I':
    case 'P':
        return readMain(marker);
    case 'G':
        return gps_ ? readSimple(marker, scratch_) : Outcome::Corrupt;
    case 'H':
        return home_ ? readSimple(marker, scratch_) : Outcome::Corrupt;
    case 'S':
        return slow_ ? readSimple(marker, scratch_) : Outcome::Corrupt;
    default:
        return readEvent();
    }
}

FrameParser::Outcome FrameParser::readMain(char marker) {
    const FrameDefinition* def = marker == 'I' ? intra_ : inter_;
    if (def == nullptr || !def->complete()) return Outcome::Corrupt;
    PredictionContext ctx;
    ctx.previous = previous_;
    if (marker == 'P') {
        ctx.previous2 = previous2_;
        skipped_ = last_ ? header_.schedule().skippedAfter(last_->iteration) : 0;
        ctx.skippedIterations = skipped_;
    }
    return decodeOutcome(decodeFields(*def, ctx, scratch_));
}

FrameParser::Outcome FrameParser::decodeOutcome(bool decoded) const {
    if (decoded) return Outcome::Accepted;
    return stats_.unsupportedEncoding || stats_.unsupportedPredictor ? Outcome::Stop : Outcome::Corrupt;
}

FrameParser::Outcome FrameParser::readSimple(char marker, std::vector<std::int64_t>& out) {
    const FrameDefinition* def = slow_;
    if (marker == 'G') def = gps_;
    if (marker == 'H') def = home_;
    if (!def->complete()) return Outcome::Corrupt;
    PredictionContext ctx;
    ctx.home = homeCoord_;
    ctx.lastMainFrameTime = last_ ? last_->timeUs : 0;
    return decodeOutcome(decodeFields(*def, ctx, out));
}

bool FrameParser::decodeFields(const FrameDefinition& def, const PredictionContext& ctx, std::vector<std::int64_t>& out) {
    if (!fields_.read(in_, def, raw_)) {
        if (fields_.unsupported()) stats_.unsupportedEncoding = fields_.unsupported();
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
    event_ = LogEvent{};
    event_.type = in_.readByte();
    switch (event_.type) {
    case kEventSyncBeep:
        event_.value = in_.readUnsignedVB();
        break;
    case kEventInflightAdjustment: {
        // Bit 7 of the function byte flags a float value, else a signed VB follows.
        const std::uint8_t function = in_.readByte();
        event_.function = function & 0x7F;
        event_.adjustment = (function & 0x80u) != 0 ? static_cast<double>(in_.readFloat())
                                                    : static_cast<double>(in_.readSignedVB());
        break;
    }
    case kEventLoggingResume:
        event_.value = in_.readUnsignedVB();
        event_.lastValue = in_.readUnsignedVB();  // the time logging resumed at
        break;
    case kEventDisarm:
        event_.value = in_.readUnsignedVB();
        break;
    case kEventFlightMode:
        event_.value = in_.readUnsignedVB();
        event_.lastValue = in_.readUnsignedVB();
        break;
    case kEventLogEnd:
        for (const char c : kEndOfLog) {
            if (in_.readByte() != static_cast<std::uint8_t>(c)) return Outcome::Corrupt;
        }
        break;
    default:
        return Outcome::Corrupt;
    }
    return in_.bad() ? Outcome::Corrupt : Outcome::Accepted;
}

void FrameParser::commit(char marker) {
    switch (marker) {
    case 'I':
    case 'P':
        commitMain(marker);
        break;
    case 'G': {
        const std::int64_t t = gpsTimeField_ && *gpsTimeField_ < scratch_.size()
                                   ? unwrapTime(static_cast<std::uint32_t>(scratch_[*gpsTimeField_]))
                                   : (last_ ? last_->timeUs : 0);
        ++stats_.frames['G'];
        sink_->onGpsFrame(scratch_, t);
        break;
    }
    case 'H':
        if (scratch_.size() >= 2) homeCoord_ = {scratch_[0], scratch_[1]};
        ++stats_.frames['H'];
        sink_->onHomeFrame(scratch_, last_ ? last_->timeUs : 0);
        break;
    case 'S':
        ++stats_.frames['S'];
        sink_->onSlowFrame(scratch_, last_ ? last_->timeUs : 0);
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
    if (!last_ || plausible(*last_, pos) || (candidate_ && plausible(*candidate_, pos))) {
        acceptMain(pos, true);
        return;
    }
    // Too far from the last good frame: wait for a second I frame that agrees with this one.
    candidate_ = pos;
    if (mainValid_ && last_) gapFromUs_ = last_->timeUs;
    mainValid_ = false;
}

void FrameParser::commitInter(const MainPosition& pos) {
    if (!mainValid_ || !last_) return;  // a P frame cannot resynchronise the stream
    if (!plausible(*last_, pos)) {
        gapFromUs_ = last_->timeUs;
        mainValid_ = false;
        return;
    }
    acceptMain(pos, false);
}

void FrameParser::acceptMain(const MainPosition& pos, bool intra) {
    if (last_ && gapFromUs_) {
        sink_->onGap(*gapFromUs_, pos.timeUs, GapCause::Corrupt);
    } else if (last_ && !resumed_ && pos.timeUs - last_->timeUs > kMissingGapUs) {
        sink_->onGap(last_->timeUs, pos.timeUs, GapCause::Missing);
    }
    gapFromUs_.reset();
    candidate_.reset();
    resumed_ = false;
    mainValid_ = true;
    while (pos.timeUs >= rolloverUs_ + kTimeWrap) rolloverUs_ += kTimeWrap;

    if (intra) {
        // After an I frame both histories are the I frame: nothing older can be trusted.
        previous_ = scratch_;
        previous2_ = scratch_;
    } else {
        previous2_ = std::move(previous_);
        previous_ = scratch_;
    }
    last_ = pos;
    ++stats_.frames[intra ? 'I' : 'P'];
    if (!stats_.firstTimeUs) stats_.firstTimeUs = pos.timeUs;
    stats_.lastTimeUs = pos.timeUs;
    sink_->onMainFrame(previous_, pos.timeUs);
}

void FrameParser::commitEvent() {
    event_.timeUs = last_ ? last_->timeUs : 0;
    switch (event_.type) {
    case kEventSyncBeep:
        event_.timeUs = unwrapTime(event_.value);
        break;
    case kEventLoggingResume: {
        // Logging was switched off (BOXBLACKBOX) and on again: frames continue from here.
        const MainPosition resumed{event_.value, unwrapTime(event_.lastValue)};
        if (last_) sink_->onGap(last_->timeUs, resumed.timeUs, GapCause::Paused);
        event_.timeUs = resumed.timeUs;
        last_ = resumed;
        resumed_ = true;
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
    if (mainValid_ && last_) gapFromUs_ = last_->timeUs;
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
    std::int64_t t = rolloverUs_ + raw;
    if (last_ && t < last_->timeUs) {
        const auto forward = static_cast<std::uint32_t>(raw - static_cast<std::uint32_t>(last_->timeUs));
        if (forward < kMaxTimeJumpUs) t += kTimeWrap;
    }
    return t;
}

}  // namespace fpv::blackbox
