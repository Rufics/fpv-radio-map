#include "log_builder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>

namespace fpv::blackbox {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCoordDivisor = 1e7;     // GPS_coord and GPS_home: degrees × 10^7
constexpr double kSpeedDivisor = 100.0;   // GPS_speed: cm/s in every release
constexpr double kCourseDivisor = 10.0;   // GPS_ground_course: degrees × 10
constexpr double kRssiMax = 1023.0;       // getRssi(): 0 to RSSI_MAX_VALUE
constexpr double kZeroCurrentA = 0.05;    // below this all flight, the sensor is not reading
constexpr std::uint32_t kArmBit = 1u;     // BOXARM is bit 0 in every release
constexpr std::int64_t kStateGpsFixHome = 1;  // stateFlags: GPS_FIX_HOME
constexpr std::int64_t kStateGpsFix = 2;      // stateFlags: GPS_FIX

json note(const char* code, const char* severity, std::string message, std::string fix = {}) {
    json n = {{"code", code}, {"severity", severity}, {"message", std::move(message)}};
    if (!fix.empty()) n["fix"] = std::move(fix);
    return n;
}

std::optional<double> coordinate(std::optional<std::int64_t> raw, double limit) {
    if (!raw.has_value()) return std::nullopt;
    const double deg = static_cast<std::int32_t>(*raw) / kCoordDivisor;
    if (std::abs(deg) > limit) return std::nullopt;
    return deg;
}

}  // namespace

void Column::push(std::optional<double> v) {
    if (v && std::isfinite(*v)) {
        data_.push_back(*v);
        ++values_;
        maxAbs_ = std::max(maxAbs_, std::abs(*v));
    } else {
        data_.push_back(kNaN);
    }
}

json Column::toJson() const {
    json out = json::array();
    for (const double v : data_) out.push_back(std::isnan(v) ? json(nullptr) : json(v));
    return out;
}

NormalizedLogBuilder::NormalizedLogBuilder(const LogHeader& header, BuildOptions options)
    : header_(header),
      model_(header),
      options_(options),
      intervalUs_(static_cast<std::int64_t>(1e6 / std::clamp(options.sampleHz, 0.1, 1000.0))) {
    if (const FrameDefinition* i = header.frame('I')) {
        main_.vbat = i->indexOf("vbatLatest");
        main_.amperage = i->indexOf("amperageLatest");
        main_.baro = i->indexOf("baroAlt");
        main_.rssi = i->indexOf("rssi");
        main_.throttleMixer = i->indexOf("setpoint[3]");
        main_.throttleRc = i->indexOf("rcCommand[3]");
        main_.debug0 = i->indexOf("debug[0]");
        main_.debug2 = i->indexOf("debug[2]");
    }
    if (const FrameDefinition* g = header.frame('G')) {
        gpsFields_.sats = g->indexOf("GPS_numSat");
        gpsFields_.lat = g->indexOf("GPS_coord[0]");
        gpsFields_.lon = g->indexOf("GPS_coord[1]");
        gpsFields_.alt = g->indexOf("GPS_altitude");
        gpsFields_.speed = g->indexOf("GPS_speed");
        gpsFields_.course = g->indexOf("GPS_ground_course");
    }
    if (const FrameDefinition* s = header.frame('S')) {
        slowFields_.modes = s->indexOf("flightModeFlags");
        slowFields_.state = s->indexOf("stateFlags");
        slowFields_.failsafe = s->indexOf("failsafePhase");
        slowFields_.rxSignal = s->indexOf("rxSignalReceived");
        slowFields_.rxValid = s->indexOf("rxFlightChannelsValid");
    }
}

std::optional<double> NormalizedLogBuilder::field(std::span<const std::int64_t> values,
                                                  const std::optional<std::size_t>& index) {
    if (const auto v = integerField(values, index); v.has_value()) return static_cast<double>(*v);
    return std::nullopt;
}

std::optional<std::int64_t> NormalizedLogBuilder::integerField(std::span<const std::int64_t> values,
                                                               const std::optional<std::size_t>& index) {
    if (!index || *index >= values.size()) return std::nullopt;
    return values[*index];
}

std::optional<double> NormalizedLogBuilder::divided(std::optional<double> raw, const std::optional<double>& divisor) {
    if (!raw.has_value() || !divisor.has_value()) return std::nullopt;
    return *raw / *divisor;
}

std::optional<double> NormalizedLogBuilder::throttlePct(std::span<const std::int64_t> values) const {
    if (!model_.unitsKnown()) return std::nullopt;
    if (model_.throttleFromMixer() && main_.throttleMixer.has_value()) {
        // From 4.0 the mixer throttle is logged in setpoint[3], scaled by a thousand.
        if (const auto v = field(values, main_.throttleMixer); v.has_value()) return std::clamp(*v / 10.0, 0.0, 100.0);
    }
    if (const auto rc = field(values, main_.throttleRc); rc.has_value()) {
        // rcCommand[THROTTLE] runs 1000..2000 µs before 4.0
        return std::clamp((*rc / model_.rcCommandScale() - 1000.0) / 10.0, 0.0, 100.0);
    }
    return std::nullopt;
}

void NormalizedLogBuilder::onMainFrame(std::span<const std::int64_t> values, std::int64_t timeUs) {
    if (!originUs_.has_value()) originUs_ = timeUs;
    if (lastKeptUs_.has_value() && !keepNext_ && timeUs - *lastKeptUs_ < intervalUs_) return;
    if (timeUs_.size() >= options_.maxSamples) {
        samplesTruncated_ = true;
        return;
    }
    lastKeptUs_ = timeUs;
    keepNext_ = false;
    timeUs_.push_back(timeUs);
    columns_.voltage.push(divided(field(values, main_.vbat), model_.voltageDivisor()));
    columns_.current.push(divided(field(values, main_.amperage), model_.currentDivisor()));
    columns_.baro.push(divided(field(values, main_.baro), model_.baroDivisor()));
    const auto rssi = field(values, main_.rssi);
    columns_.rssi.push(rssi && model_.unitsKnown() ? std::optional<double>(*rssi / kRssiMax * 100.0) : std::nullopt);
    columns_.throttle.push(throttlePct(values));
    if (model_.linkStatisticsInDebug()) {
        // crsf.c: DEBUG_SET(DEBUG_CRSF_LINK_STATISTICS_UPLINK, 0, uplink RSSI) and (…, 2, link quality).
        const auto dbm = field(values, main_.debug0);
        columns_.linkQuality.push(field(values, main_.debug2));
        columns_.rssiDbm.push(dbm ? std::optional<double>(-std::abs(*dbm)) : std::nullopt);
    }
}

void NormalizedLogBuilder::onGpsFrame(std::span<const std::int64_t> values, std::int64_t timeUs) {
    GpsSample s;
    s.timeUs = timeUs;
    s.lat = coordinate(integerField(values, gpsFields_.lat), 90.0);
    s.lon = coordinate(integerField(values, gpsFields_.lon), 180.0);
    if (s.lat.has_value() && s.lon.has_value() && *s.lat == 0.0 && *s.lon == 0.0) {
        s.lat.reset();
        s.lon.reset();
    }
    s.sats = field(values, gpsFields_.sats);
    if (model_.unitsKnown()) {
        // GPS_altitude was unsigned before 4.5 but carried a signed altitude: read it as int32.
        const auto alt = integerField(values, gpsFields_.alt);
        if (const auto divisor = model_.gpsAltitudeDivisor(); alt.has_value() && divisor.has_value()) {
            s.alt = static_cast<std::int32_t>(*alt) / *divisor;
        }
        s.speed = divided(field(values, gpsFields_.speed), kSpeedDivisor);
        s.course = divided(field(values, gpsFields_.course), kCourseDivisor);
    }
    // The slow frame's GPS_FIX flag says whether this solution is a fix; without slow frames
    // only a position counts.
    s.fix = s.lat.has_value() && state_.gpsFix.value_or(true);
    gps_.push_back(s);
}

void NormalizedLogBuilder::onHomeFrame(std::span<const std::int64_t> values, std::int64_t timeUs) {
    if (values.size() < 2 || (values[0] == 0 && values[1] == 0)) return;
    const auto lat = coordinate(values[0], 90.0);
    const auto lon = coordinate(values[1], 180.0);
    if (!lat.has_value() || !lon.has_value()) return;
    json home = {{"lat", *lat}, {"lon", *lon}};
    // 2025.12 added GPS_home[2], the home altitude in decimetres.
    if (values.size() >= 3 && model_.firmware().betaflightAtLeast({2025, 12, 0})) {
        home["altM"] = static_cast<std::int32_t>(values[2]) / 10.0;
    }
    if (!homes_.empty() && homes_.back().second == home) return;
    homes_.emplace_back(timeUs, std::move(home));
}

void NormalizedLogBuilder::applyModes(State& s, std::uint32_t mask) const {
    if (const ModeTable* table = model_.modes()) {
        s.armed = (mask & kArmBit) != 0;
        std::vector<std::string> names = table->active(mask & ~kArmBit);
        s.modes = std::move(names);
    }
}

void NormalizedLogBuilder::onSlowFrame(std::span<const std::int64_t> values, std::int64_t timeUs) {
    State s = state_;
    s.timeUs = timeUs;
    if (const auto mask = integerField(values, slowFields_.modes); mask.has_value()) {
        applyModes(s, static_cast<std::uint32_t>(*mask));
    }
    if (model_.unitsKnown()) {
        if (const auto flags = integerField(values, slowFields_.state); flags.has_value()) {
            s.gpsFix = (*flags & kStateGpsFix) != 0;
            s.gpsHomeSet = (*flags & kStateGpsFixHome) != 0;
        }
        if (const auto phase = integerField(values, slowFields_.failsafe); phase.has_value()) {
            s.failsafe = FirmwareModel::failsafePhase(*phase);
        }
    }
    if (const auto rx = field(values, slowFields_.rxSignal); rx.has_value()) s.rxSignal = *rx != 0;
    if (const auto rx = field(values, slowFields_.rxValid); rx.has_value()) s.rxValid = *rx != 0;
    pushState(std::move(s));
}

void NormalizedLogBuilder::pushState(State s) {
    state_ = s;
    if (!states_.empty() && states_.back().sameAs(s)) return;
    states_.push_back(std::move(s));
}

void NormalizedLogBuilder::onEvent(const LogEvent& event) {
    switch (event.type) {
    case kEventSyncBeep:
        events_.push_back({event.timeUs, {{"type", "sync-beep"}}});
        break;
    case kEventDisarm:
        events_.push_back({event.timeUs, {{"type", "disarm"}, {"reason", FirmwareModel::disarmReason(event.value)}}});
        break;
    case kEventInflightAdjustment:
        events_.push_back({event.timeUs,
                           {{"type", "inflight-adjustment"}, {"function", event.function}, {"value", event.adjustment}}});
        break;
    case kEventFlightMode: {
        State s = state_;
        s.timeUs = event.timeUs;
        applyModes(s, event.value);
        pushState(std::move(s));
        break;
    }
    case kEventLogEnd:
        events_.push_back({event.timeUs, {{"type", "log-end"}}});
        break;
    default:
        break;  // LOGGING_RESUME arrives as a gap
    }
}

void NormalizedLogBuilder::onGap(std::int64_t fromUs, std::int64_t toUs, GapCause cause) {
    const double duration = static_cast<double>(std::max<std::int64_t>(toUs - fromUs, 0)) / 1e6;
    keepNext_ = true;
    if (cause == GapCause::Paused) {
        events_.push_back({fromUs, {{"type", "logging-paused"}, {"durationS", duration}}});
        return;
    }
    ++gaps_;
    lostS_ += duration;
    events_.push_back(
        {fromUs, {{"type", "gap"}, {"reason", cause == GapCause::Corrupt ? "corrupt" : "missing"}, {"durationS", duration}}});
}

double NormalizedLogBuilder::seconds(std::int64_t timeUs) const {
    if (!originUs_.has_value() || timeUs <= *originUs_) return 0.0;
    return static_cast<double>(timeUs - *originUs_) / 1e6;
}

json NormalizedLogBuilder::firmwareJson() const {
    const FirmwareInfo& fw = model_.firmware();
    json out = {{"name", fw.name()}, {"supported", model_.unitsKnown()}};
    if (const auto& version = fw.version()) out["version"] = version->str();
    if (!fw.revision().empty()) out["revision"] = fw.revision();
    if (!fw.target().empty()) out["target"] = fw.target();
    if (!fw.board().empty()) out["board"] = fw.board();
    if (!fw.buildDate().empty()) out["buildDate"] = fw.buildDate();
    return out;
}

json NormalizedLogBuilder::channelsJson() const {
    const bool currentReads = columns_.current.anyValue() && columns_.current.maxAbs() >= kZeroCurrentA;
    const bool anyFix = std::ranges::any_of(gps_, [](const GpsSample& s) { return s.fix; });
    return {{"gps", anyFix},
            {"voltage", columns_.voltage.anyValue()},
            {"current", currentReads},
            {"baroAltitude", columns_.baro.anyValue()},
            {"rssi", columns_.rssi.anyValue() && columns_.rssi.maxAbs() > 0.0},
            {"linkQuality", columns_.linkQuality.anyValue()},
            {"rssiDbm", columns_.rssiDbm.anyValue()},
            {"throttle", columns_.throttle.anyValue()},
            {"modes", model_.modes() != nullptr && !states_.empty()}};
}

json NormalizedLogBuilder::seriesJson() const {
    json t = json::array();
    for (const std::int64_t us : timeUs_) t.push_back(seconds(us));
    json out = {{"t", std::move(t)}};
    const auto add = [&out](const char* name, const Column& c) {
        if (c.anyValue()) out[name] = c.toJson();
    };
    add("voltageV", columns_.voltage);
    if (columns_.current.maxAbs() >= kZeroCurrentA) add("currentA", columns_.current);
    add("baroAltM", columns_.baro);
    if (columns_.rssi.maxAbs() > 0.0) add("rssiPct", columns_.rssi);
    add("linkQualityPct", columns_.linkQuality);
    add("rssiDbm", columns_.rssiDbm);
    add("throttlePct", columns_.throttle);
    return out;
}

json NormalizedLogBuilder::gpsJson() const {
    json t = json::array();
    json fix = json::array();
    Column lat;
    Column lon;
    Column alt;
    Column speed;
    Column course;
    Column sats;
    for (const GpsSample& s : gps_) {
        t.push_back(seconds(s.timeUs));
        fix.push_back(s.fix);
        lat.push(s.lat);
        lon.push(s.lon);
        alt.push(s.alt);
        speed.push(s.speed);
        course.push(s.course);
        sats.push(s.sats);
    }
    json out = {{"t", std::move(t)}, {"lat", lat.toJson()}, {"lon", lon.toJson()}, {"fix", std::move(fix)}};
    if (alt.anyValue()) out["altM"] = alt.toJson();
    if (speed.anyValue()) out["speedMps"] = speed.toJson();
    if (course.anyValue()) out["courseDeg"] = course.toJson();
    if (sats.anyValue()) out["sats"] = sats.toJson();
    return out;
}

json NormalizedLogBuilder::statesJson() const {
    json out = json::array();
    for (const State& s : states_) {
        json e = {{"t", seconds(s.timeUs)}};
        if (s.armed.has_value()) e["armed"] = *s.armed;
        if (s.modes) e["modes"] = *s.modes;
        if (s.failsafe) e["failsafe"] = *s.failsafe;
        if (s.rxSignal.has_value()) e["rxSignal"] = *s.rxSignal;
        if (s.rxValid.has_value()) e["rxValid"] = *s.rxValid;
        if (s.gpsFix.has_value()) e["gpsFix"] = *s.gpsFix;
        if (s.gpsHomeSet.has_value()) e["gpsHomeSet"] = *s.gpsHomeSet;
        out.push_back(std::move(e));
    }
    return out;
}

json NormalizedLogBuilder::eventsJson() const {
    json out = json::array();
    for (const Event& e : events_) {
        json body = e.body;
        body["t"] = seconds(e.timeUs);
        out.push_back(std::move(body));
    }
    return out;
}

void NormalizedLogBuilder::addSensorNotes(json& notes) const {
    if (!main_.amperage.has_value()) {
        notes.push_back(note("no-current-sensor", "info", "This craft logs no current sensor, so mAh and Wh are not shown.",
                             "Set current_meter to ADC or ESC and calibrate ibata_scale to log the current."));
    } else if (columns_.current.anyValue() && columns_.current.maxAbs() < kZeroCurrentA) {
        notes.push_back(note("current-reads-zero", "info", "The current sensor read 0 A all flight, so mAh and Wh are not shown.",
                             "Calibrate the current sensor (ibata_scale) or check current_meter."));
    }
    if (main_.rssi.has_value() && columns_.rssi.anyValue() && columns_.rssi.maxAbs() == 0.0) {
        notes.push_back(note("rssi-not-set", "info", "RSSI read 0 all flight: no RSSI source is set.",
                             "Set an RSSI source in the Receiver tab (an RSSI channel, or from the receiver protocol)."));
    }
}

void NormalizedLogBuilder::addGpsNotes(json& notes) const {
    if (gps_.empty()) {
        notes.push_back(note("no-gps", "info", "No GPS data in this log, so the flight cannot be placed on the map."));
        return;
    }
    const auto fixed = std::ranges::count_if(gps_, [](const GpsSample& s) { return s.fix; });
    if (fixed == 0) {
        notes.push_back(note("gps-no-fix", "warn", "The GPS never had a fix during this log, so there is no track to show.",
                             "Wait for the GPS fix (and the home point) before arming."));
        return;
    }
    double withoutFixS = 0.0;
    for (std::size_t i = 1; i < gps_.size(); ++i) {
        if (!gps_[i - 1].fix) withoutFixS += seconds(gps_[i].timeUs) - seconds(gps_[i - 1].timeUs);
    }
    if (static_cast<std::size_t>(fixed) < gps_.size()) {
        notes.push_back(note("gps-fix-lost", "info",
                             std::format("The GPS had no fix for {:.1f} s of the log; the track skips those points.", withoutFixS)));
    }
}

void NormalizedLogBuilder::addLinkNotes(json& notes) const {
    if (!model_.crsf() || model_.linkStatisticsInDebug()) return;
    if (model_.linkStatisticsDebugAvailable()) {
        notes.push_back(note("link-quality-missing", "info",
                             "The receiver is CRSF but this log has RSSI only, not link quality.",
                             "Set debug_mode = CRSF_LINK_STATISTICS_UPLINK so the next log records link quality and RSSI in dBm."));
    } else {
        notes.push_back(note("link-quality-missing", "info",
                             "The receiver is CRSF but this log has RSSI only, not link quality.",
                             "Betaflight 4.1 or newer can log link quality with debug_mode = CRSF_LINK_STATISTICS_UPLINK."));
    }
}

json NormalizedLogBuilder::notesJson(const ParseStats& stats) const {
    json notes = json::array();
    const bool hasFrames = !timeUs_.empty() || !gps_.empty();
    if (stats.unsupportedEncoding.has_value() || stats.unsupportedPredictor.has_value()) {
        const std::string what = stats.unsupportedEncoding.has_value() ? std::format("field encoding {}", *stats.unsupportedEncoding)
                                                           : std::format("field predictor {}", *stats.unsupportedPredictor);
        notes.push_back(note("unsupported-format", "blocked", "The log uses " + what + ", which this decoder does not know."));
    }
    if (!hasFrames && !stats.unsupportedEncoding.has_value() && !stats.unsupportedPredictor.has_value()) {
        notes.push_back(note("header-only", "warn", "The log has its header but no flight data: logging stopped right after it started.",
                             "Check that Blackbox logging stays on while armed and that the flash or card is not full."));
        return notes;
    }
    if (!model_.unitsKnown()) {
        const FirmwareInfo& fw = model_.firmware();
        const auto& version = fw.version();
        notes.push_back(note("firmware-units-unknown", "warn",
                             std::format("{}{} logs are read for the GPS track only: battery, altitude, link and modes use units "
                                         "this decoder does not know for that firmware.",
                                         fw.name(), version ? " " + version->str() : std::string())));
    }
    if (gaps_ > 0) {
        notes.push_back(note("corrupt-frames", "warn",
                             std::format("The log has {} gap{} of damaged or missing frames, {:.2f} s in all; the replay "
                                         "shows {} as no data.",
                                         gaps_, gaps_ == 1 ? "" : "s", lostS_, gaps_ == 1 ? "it" : "them")));
    } else if (stats.corruptFrames > 0) {
        notes.push_back(note("corrupt-frames", "info",
                             std::format("{} damaged frame{} skipped without losing flight data.", stats.corruptFrames,
                                         stats.corruptFrames == 1 ? " was" : "s were")));
    }
    if (hasFrames && !stats.logEnd) {
        notes.push_back(note("log-truncated", "info",
                             "The log stops without its end marker: power was cut or the storage filled before logging ended."));
    }
    if (samplesTruncated_) {
        notes.push_back(note("samples-truncated", "warn",
                             std::format("Only the first {} samples are kept; the rest of the log is not shown.", options_.maxSamples)));
    }
    addSensorNotes(notes);
    addGpsNotes(notes);
    addLinkNotes(notes);
    return notes;
}

std::optional<std::string> NormalizedLogBuilder::startedAt() const {
    // "Log start datetime" is "0000-01-01T00:00:00.000+00:00" when the clock was never set.
    const auto v = header_.value("Log start datetime");
    if (!v.has_value() || v->size() < 20 || v->starts_with("0000")) return std::nullopt;
    const std::string& s = *v;
    if (const bool shape = std::isdigit(static_cast<unsigned char>(s[0])) && s[4] == '-' && s[7] == '-' && s[10] == 'T' &&
                           s[13] == ':' && s[16] == ':';
        !shape || s.size() > 40) {
        return std::nullopt;
    }
    return s;
}

std::string NormalizedLogBuilder::status(const ParseStats& stats) const {
    if (stats.unsupportedEncoding.has_value() || stats.unsupportedPredictor.has_value()) return "unsupported";
    if (timeUs_.empty() && gps_.empty()) return "header-only";
    if (stats.corruptFrames > 0 || !model_.unitsKnown() || samplesTruncated_) return "partial";
    return "ok";
}

json NormalizedLogBuilder::finish(const ParseStats& stats, int index, std::pair<std::size_t, std::size_t> byteRange) const {
    json log = {{"index", index},
                {"status", status(stats)},
                {"byteRange", {byteRange.first, byteRange.second}},
                {"firmware", firmwareJson()},
                {"channels", channelsJson()},
                {"notes", notesJson(stats)}};
    if (const auto craft = header_.value("Craft name"); craft.has_value() && !craft->empty()) log["craft"] = craft->substr(0, 128);
    if (const auto at = startedAt(); at.has_value()) log["startedAt"] = *at;
    if (stats.firstTimeUs.has_value() && stats.lastTimeUs.has_value()) {
        log["durationS"] = static_cast<double>(*stats.lastTimeUs - *stats.firstTimeUs) / 1e6;
    }
    const auto divisor = model_.voltageDivisor();
    if (const auto cells = header_.integers("vbatcellvoltage"); cells.size() >= 3 && divisor.has_value()) {
        const double d = *divisor;
        log["battery"] = {{"cellMinV", static_cast<double>(cells[0]) / d},
                          {"cellWarnV", static_cast<double>(cells[1]) / d},
                          {"cellMaxV", static_cast<double>(cells[2]) / d}};
    }
    if (const std::string protocol = model_.radioProtocol(); !protocol.empty()) log["radio"] = {{"protocol", protocol}};
    if (!timeUs_.empty()) log["series"] = seriesJson();
    if (!gps_.empty()) log["gps"] = gpsJson();
    if (!homes_.empty()) log["homes"] = homesJson();
    if (!states_.empty()) log["states"] = statesJson();
    if (!events_.empty()) log["events"] = eventsJson();
    log["stats"] = statsJson(stats);
    return log;
}

json NormalizedLogBuilder::homesJson() const {
    json homes = json::array();
    for (const auto& [t, home] : homes_) {
        json h = home;
        h["t"] = seconds(t);
        homes.push_back(std::move(h));
    }
    return homes;
}

json NormalizedLogBuilder::statsJson(const ParseStats& stats) const {
    json frames = json::object();
    for (const auto& [marker, count] : stats.frames) frames[std::string(1, marker)] = count;
    json out = {{"frames", std::move(frames)}, {"corruptFrames", stats.corruptFrames}, {"sampleHz", options_.sampleHz}};
    if (stats.firstTimeUs.has_value() && stats.lastTimeUs.has_value() && *stats.lastTimeUs > *stats.firstTimeUs) {
        const auto mainFrames = stats.frames.contains('I') ? stats.frames.at('I') : 0;
        const auto interFrames = stats.frames.contains('P') ? stats.frames.at('P') : 0;
        out["loggedHz"] =
            static_cast<double>(mainFrames + interFrames) / (static_cast<double>(*stats.lastTimeUs - *stats.firstTimeUs) / 1e6);
    }
    return out;
}

}  // namespace fpv::blackbox
