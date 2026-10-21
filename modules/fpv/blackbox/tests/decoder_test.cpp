// fpv.blackbox decoding: encodings and predictors against the firmware's writers, the version
// tables, and whole logs with the edge cases a pilot's file has (no GPS fix, damaged frames,
// several flights, no current sensor, CRSF without link quality, other firmware).

#include "decoder.hpp"
#include "firmware_model.hpp"
#include "frame_decoder.hpp"
#include "sample_log.hpp"
#include "synthetic_log.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>

using namespace fpv::blackbox;
using fpv::blackbox::test::MainValues;
using fpv::blackbox::test::SyntheticLog;

namespace {

json decodeAll(const std::vector<std::uint8_t>& bytes, double sampleHz = 1000.0) {
    BuildOptions o;
    o.sampleHz = sampleHz;
    return BlackboxDecoder("fpv.blackbox", "1.0.0").decode(bytes, "test.bbl", o);
}

json decodeOne(const std::vector<std::uint8_t>& bytes, double sampleHz = 1000.0) {
    const json all = decodeAll(bytes, sampleHz);
    EXPECT_EQ(all.at("logs").size(), 1u);
    return all.at("logs").at(0);
}

bool hasNote(const json& log, const std::string& code) {
    return std::ranges::any_of(log.at("notes"), [&code](const json& n) { return n.at("code") == code; });
}

LogHeader headerOf(const std::vector<std::pair<std::string, std::string>>& lines) {
    LogWriter w;
    for (const auto& [name, value] : lines) w.headerLine(name, value);
    ByteReader r(w.bytes());
    return LogHeader::read(r);
}

FrameDefinition definition(const std::string& encodings) {
    const auto codes = LogHeader::splitIntegers(encodings);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < codes.size(); ++i) names.push_back("f" + std::to_string(i));
    FrameDefinition def;
    def.setNames(names);
    def.setPredictors(std::vector<long long>(codes.size(), 0));
    def.setEncodings(codes);
    return def;
}

std::vector<std::int64_t> readFields(const std::vector<std::uint8_t>& bytes, const std::string& encodings, int dataVersion = 2) {
    ByteReader r(bytes);
    FieldReader reader(dataVersion);
    std::vector<std::int64_t> raw;
    EXPECT_TRUE(reader.read(r, definition(encodings), raw));
    EXPECT_TRUE(r.atEnd()) << "bytes left after the frame";
    return raw;
}

}  // namespace

TEST(ByteReader, VariableByteNumbers) {
    LogWriter w;
    w.unsignedVB(0);
    w.unsignedVB(300);
    w.unsignedVB(0xFFFFFFFFu);
    w.signedVB(-1);
    w.signedVB(-2147483647 - 1);
    ByteReader r(w.bytes());
    EXPECT_EQ(r.readUnsignedVB(), 0u);
    EXPECT_EQ(r.readUnsignedVB(), 300u);
    EXPECT_EQ(r.readUnsignedVB(), 0xFFFFFFFFu);
    EXPECT_EQ(r.readSignedVB(), -1);
    EXPECT_EQ(r.readSignedVB(), -2147483647 - 1);
    EXPECT_FALSE(r.bad());
    EXPECT_EQ(r.readByte(), 0);
    EXPECT_TRUE(r.bad());

    const std::vector<std::uint8_t> tooLong = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    ByteReader bad(tooLong);
    EXPECT_EQ(bad.readUnsignedVB(), 0u);
    EXPECT_TRUE(bad.bad());
}

TEST(FieldReader, Tag2_3S32EveryWidth) {
    for (const std::array<std::int32_t, 3> v : {std::array<std::int32_t, 3>{1, -2, 0}, {7, -8, 3}, {31, -32, 9},
                                                 {100, -30000, 8000000}, {-2147483647, 2147483647, 0}}) {
        LogWriter w;
        w.tag2_3S32(v);
        const auto raw = readFields(w.bytes(), "7,7,7");
        EXPECT_EQ(raw, (std::vector<std::int64_t>{v[0], v[1], v[2]}));
    }
}

TEST(FieldReader, Tag8_4S16NibblesAcrossBytes) {
    // 4-bit then 16-bit: the 16-bit value starts in the low nibble of a byte.
    for (const std::array<std::int32_t, 4> v : {std::array<std::int32_t, 4>{0, 0, 0, 0}, {3, -1200, 0, 7}, {-8, 127, -128, 32767},
                                                 {5, 6, -32768, 100}}) {
        LogWriter w;
        w.tag8_4S16(v);
        const auto raw = readFields(w.bytes(), "8,8,8,8");
        EXPECT_EQ(raw, (std::vector<std::int64_t>{v[0], v[1], v[2], v[3]}));
    }
}

TEST(FieldReader, Tag8_4S16DataVersion1) {
    // Selector 0b11'10'01'01: two 4-bit fields in one byte (low nibble first), 8-bit, 16-bit LE.
    const std::vector<std::uint8_t> bytes = {0xE5, 0x2F, 0x80, 0x34, 0x12};
    const auto raw = readFields(bytes, "8,8,8,8", 1);
    EXPECT_EQ(raw, (std::vector<std::int64_t>{-1, 2, -128, 0x1234}));
}

TEST(FieldReader, Tag8_8SVBGroupsConsecutiveFields) {
    LogWriter w;
    const std::vector<std::int32_t> group = {5, 0, -3, 0, 0, 1000};
    w.tag8_8SVB(group);
    w.tag8_8SVB(std::vector<std::int32_t>{-7});  // a lone field is a plain signed VB
    const auto raw = readFields(w.bytes(), "6,6,6,6,6,6,0");
    EXPECT_EQ(raw, (std::vector<std::int64_t>{5, 0, -3, 0, 0, 1000, -7}));
}

TEST(FieldReader, Tag2_3SVariableAndNeg14Bit) {
    // 5-5-4 bits: v0 = -16, v1 = 15, v2 = -8 → ss 10000 01111 1000.
    const std::vector<std::uint8_t> s554 = {0x40 | (0x10 << 1) | (0x0F >> 4), ((0x0F & 0x0F) << 4) | 0x08};
    EXPECT_EQ(readFields(s554, "10,10,10"), (std::vector<std::int64_t>{-16, 15, -8}));
    // 8-7-7 bits: v0 = 200 - 256 = -56 (0xC8), v1 = -64 (0x40), v2 = 63.
    const std::vector<std::uint8_t> s877 = {0x80 | (0xC8 >> 2), ((0xC8 & 3) << 6) | (0x40 >> 1), ((0x40 & 1) << 7) | 63};
    EXPECT_EQ(readFields(s877, "10,10,10"), (std::vector<std::int64_t>{-56, -64, 63}));

    LogWriter w;
    w.unsignedVB(static_cast<std::uint32_t>(1680 - 1600) & 0x3FFFu);  // vbatref − vbat, 14 bits
    EXPECT_EQ(readFields(w.bytes(), "3"), (std::vector<std::int64_t>{-80}));
}

TEST(FieldReader, UnknownEncodingIsReported) {
    const std::vector<std::uint8_t> bytes = {1};
    ByteReader r(bytes);
    FieldReader reader(2);
    std::vector<std::int64_t> raw;
    EXPECT_FALSE(reader.read(r, definition("4"), raw));
    EXPECT_EQ(reader.unsupported(), 4);
}

TEST(Predictors, FirmwarePredictions) {
    const LogHeader header = headerOf({{"minthrottle", "1070"}, {"vbatref", "2362"}, {"motorOutput", "158,2047"}});
    FrameDefinition def;
    def.setNames({"a", "b", "c", "d", "motor[0]", "motor[1]", "e", "GPS_coord[0]", "GPS_coord[1]", "f", "g", "h", "i"});
    def.setSigned({1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0});
    def.setPredictors({1, 2, 3, 4, 11, 5, 6, 7, 7, 8, 9, 10, 0});
    def.setEncodings(std::vector<long long>(13, 0));
    Predictors p(header);
    const std::vector<std::int64_t> prev = {10, 10, -3, 0, 0, 0, 41, 0, 0, 0, 0, 0, 0};
    const std::vector<std::int64_t> prev2 = {0, 4, -4, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0};
    PredictionContext ctx{prev, prev2, 1, 5000, {500000000, 300000000}};
    std::vector<std::int64_t> v = {1, 1, 0, 0, 2, 3, 0, 10, -10, 1, 0, 7, -5};
    ASSERT_TRUE(p.apply(def, ctx, v));
    // (-3 + -4) / 2 truncates toward zero like C: -3.
    EXPECT_EQ(v, (std::vector<std::int64_t>{11, 17, -3, 1070, 160, 163, 43, 500000010, 299999990, 1501, 2362, 5007, -5 + (1LL << 32)}));
}

TEST(FrameSchedule, ModernAndLegacyIntervals) {
    // Betaflight "I interval:102", "P interval:2": every other iteration, an I frame every 102.
    const FrameSchedule modern(102, 1, 2);
    EXPECT_TRUE(modern.logged(0));
    EXPECT_FALSE(modern.logged(1));
    EXPECT_TRUE(modern.logged(100));
    EXPECT_FALSE(modern.logged(101));
    EXPECT_TRUE(modern.logged(102));
    EXPECT_EQ(modern.skippedAfter(0), 1u);
    EXPECT_EQ(modern.skippedAfter(100), 1u);
    // Cleanflight "P interval:1/4".
    const FrameSchedule legacy(32, 1, 4);
    EXPECT_EQ(legacy.skippedAfter(0), 3u);

    const LogHeader header = headerOf({{"I interval", "102"}, {"P interval", "2"}});
    EXPECT_EQ(header.schedule().skippedAfter(2), 1u);
}

TEST(FirmwareInfo, ReadsTheRevisionLine) {
    const LogHeader bf = headerOf({{"Firmware type", "Cleanflight"}, {"Firmware revision", "Betaflight 4.5.1 (77d01ba3b) STM32F7X2"},
                                   {"Board information", "SPBE SPEEDYBEEF7V3"}});
    EXPECT_EQ(bf.firmware().kind(), FirmwareKind::Betaflight);
    EXPECT_EQ(bf.firmware().version(), (Version{4, 5, 1}));
    EXPECT_EQ(bf.firmware().revision(), "77d01ba3b");
    EXPECT_EQ(bf.firmware().target(), "STM32F7X2");
    EXPECT_EQ(bf.firmware().board(), "SPBE SPEEDYBEEF7V3");

    const LogHeader dated = headerOf({{"Firmware revision", "Betaflight 2025.12.1 (abcdef0) STM32H743"}});
    EXPECT_EQ(dated.firmware().version(), (Version{2025, 12, 1}));
    const LogHeader inav = headerOf({{"Firmware revision", "INAV 7.1.0 (a1b2c3d) MATEKF405"}});
    EXPECT_EQ(inav.firmware().kind(), FirmwareKind::Inav);
    const LogHeader old = headerOf({{"Firmware type", "Cleanflight"}, {"Firmware revision", "c4b9f1e"}});
    EXPECT_EQ(old.firmware().kind(), FirmwareKind::Cleanflight);
    EXPECT_FALSE(old.firmware().version().has_value());
    EXPECT_EQ(old.firmware().revision(), "c4b9f1e");
}

TEST(ModeTable, BitsMeanDifferentModesPerRelease) {
    const auto modesOf = [](const char* revision, std::uint32_t mask) {
        const LogHeader h = headerOf({{"Firmware revision", revision}});
        const ModeTable* t = ModeTable::forFirmware(h.firmware());
        return t ? t->active(mask) : std::vector<std::string>{"none"};
    };
    // Bit 4: HEADFREE in 4.5, ALTHOLD from 2025.12. Bit 3: BARO in 3.2, MAG in 3.3 and later.
    EXPECT_EQ(modesOf("Betaflight 4.5.1 (x) T", 1u << 4), std::vector<std::string>{"HEADFREE"});
    EXPECT_EQ(modesOf("Betaflight 2025.12.1 (x) T", 1u << 4), std::vector<std::string>{"ALTHOLD"});
    EXPECT_EQ(modesOf("Betaflight 3.2.5 (x) T", 1u << 3), std::vector<std::string>{"BARO"});
    EXPECT_EQ(modesOf("Betaflight 3.3.0 (x) T", 1u << 3), std::vector<std::string>{"MAG"});
    EXPECT_EQ(modesOf("Betaflight 4.5.1 (x) T", 0b11), (std::vector<std::string>{"ARM", "ANGLE"}));
    EXPECT_EQ(modesOf("Betaflight 4.5.1 (x) T", 1u << 7), std::vector<std::string>{"GPSRESCUE"});
    EXPECT_EQ(modesOf("Betaflight 2025.12.1 (x) T", 1u << 10), std::vector<std::string>{"GPSRESCUE"});
    EXPECT_EQ(modesOf("Betaflight 3.0.1 (x) T", 1), std::vector<std::string>{"none"});
    EXPECT_EQ(modesOf("INAV 7.1.0 (x) T", 1), std::vector<std::string>{"none"});
}

TEST(FirmwareModel, UnitsFollowTheRelease) {
    const auto model = [](const char* revision) { return FirmwareModel(headerOf({{"Firmware revision", revision}})); };
    EXPECT_EQ(model("Betaflight 3.2.0 (x) T").gpsAltitudeDivisor(), 1.0);
    EXPECT_EQ(model("Betaflight 3.5.7 (x) T").gpsAltitudeDivisor(), 100.0);
    EXPECT_EQ(model("Betaflight 4.0.0 (x) T").gpsAltitudeDivisor(), 10.0);
    EXPECT_EQ(model("Betaflight 3.5.7 (x) T").voltageDivisor(), 10.0);
    EXPECT_EQ(model("Betaflight 4.5.1 (x) T").voltageDivisor(), 100.0);
    EXPECT_FALSE(model("Betaflight 3.5.7 (x) T").throttleFromMixer());
    EXPECT_FALSE(model("INAV 7.1.0 (x) T").voltageDivisor().has_value());
    EXPECT_EQ(FirmwareModel::disarmReason(4), "SWITCH");
    EXPECT_EQ(FirmwareModel::disarmReason(42), "REASON_42");
    EXPECT_EQ(FirmwareModel::failsafePhase(6), "GPS_RESCUE");
}

TEST(Base64, DecodesAndRejects) {
    EXPECT_EQ(Base64::decode("SGVsbG8="), (std::vector<std::uint8_t>{'H', 'e', 'l', 'l', 'o'}));
    EXPECT_EQ(Base64::decode("SGVs\nbG8h"), (std::vector<std::uint8_t>{'H', 'e', 'l', 'l', 'o', '!'}));
    EXPECT_FALSE(Base64::decode("SGV$").has_value());
    EXPECT_FALSE(Base64::decode("S").has_value());
    EXPECT_FALSE(Base64::decode("SG=x").has_value());
}

TEST(BlackboxDecoder, AFlightWithGpsModesAndDisarm) {
    SyntheticLog log;
    log.slow(0b11, 0b111, 0);  // ARM + ANGLE, GPS fix and home
    log.fly(0.05);
    log.home(485001652, 352007737);
    log.gps({12, 485001652, 352007737, 1192, 0, 0});
    log.fly(0.05, 5, 1'050'000);
    log.gps({13, 485002652, 352008737, 1250, 850, 900});
    log.flightMode(0b1, 0b11);  // ANGLE off: acro
    log.disarm(4);
    log.slow(0, 0b111, 0);
    log.end();

    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "ok") << l.at("notes").dump();
    EXPECT_EQ(l.at("craft"), "TestQuad");
    EXPECT_EQ(l.at("startedAt"), "2024-09-22T19:43:32.234+00:00");
    EXPECT_EQ(l.at("firmware").at("version"), "4.5.1");
    EXPECT_DOUBLE_EQ(l.at("durationS").get<double>(), 0.09);
    EXPECT_EQ(l.at("battery").at("cellWarnV"), 3.5);

    const json& s = l.at("series");
    ASSERT_EQ(s.at("t").size(), 10u);
    EXPECT_DOUBLE_EQ(s.at("voltageV").at(0).get<double>(), 16.8);
    EXPECT_DOUBLE_EQ(s.at("currentA").at(3).get<double>(), 10.0);
    EXPECT_NEAR(s.at("rssiPct").at(0).get<double>(), 87.98, 0.01);
    EXPECT_DOUBLE_EQ(s.at("throttlePct").at(0).get<double>(), 50.0);

    const json& g = l.at("gps");
    ASSERT_EQ(g.at("t").size(), 2u);
    EXPECT_DOUBLE_EQ(g.at("lat").at(1).get<double>(), 48.5002652);
    EXPECT_DOUBLE_EQ(g.at("lon").at(1).get<double>(), 35.2008737);
    EXPECT_DOUBLE_EQ(g.at("altM").at(1).get<double>(), 125.0);
    EXPECT_DOUBLE_EQ(g.at("speedMps").at(1).get<double>(), 8.5);
    EXPECT_DOUBLE_EQ(g.at("courseDeg").at(1).get<double>(), 90.0);
    EXPECT_TRUE(g.at("fix").at(1).get<bool>());
    EXPECT_DOUBLE_EQ(g.at("t").at(0).get<double>(), 0.04);  // the main frame it follows
    EXPECT_EQ(l.at("homes").size(), 1u);

    const json& states = l.at("states");
    ASSERT_GE(states.size(), 3u);
    EXPECT_EQ(states.at(0).at("modes"), json::array({"ANGLE"}));
    EXPECT_TRUE(states.at(0).at("armed").get<bool>());
    EXPECT_EQ(states.at(1).at("modes"), json::array());
    EXPECT_FALSE(states.back().at("armed").get<bool>());

    const json& events = l.at("events");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events.at(0).at("type"), "disarm");
    EXPECT_EQ(events.at(0).at("reason"), "SWITCH");
    EXPECT_EQ(events.at(1).at("type"), "log-end");
    EXPECT_FALSE(hasNote(l, "log-truncated"));
}

TEST(BlackboxDecoder, SampleRateThinsTheMainLoopOnly) {
    SyntheticLog log;
    log.fly(2.0);
    log.end();
    const json l = decodeOne(log.bytes(), 10.0);
    EXPECT_EQ(l.at("series").at("t").size(), 20u);
    EXPECT_DOUBLE_EQ(l.at("series").at("t").at(1).get<double>(), 0.1);
    EXPECT_EQ(l.at("stats").at("frames").at("I").get<int>() + l.at("stats").at("frames").at("P").get<int>(), 200);
}

TEST(BlackboxDecoder, HeaderOnlyLog) {
    SyntheticLog log;
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "header-only");
    EXPECT_TRUE(hasNote(l, "header-only"));
    EXPECT_FALSE(l.contains("series"));
    EXPECT_EQ(l.at("craft"), "TestQuad");
}

TEST(BlackboxDecoder, CorruptBytesBecomeAGapAndTheStreamResyncs) {
    SyntheticLog log;
    log.fly(0.64);  // 64 frames, I frames at 0 and 32
    // Damage in the middle of a P stretch: a P marker and bytes that do not end at a frame marker.
    log.raw({'P', 0xFF, 0xFF, 0x7A, 'q', 'z'});
    log.fly(0.64, 70, 1'700'000);
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "partial");
    EXPECT_GE(l.at("stats").at("corruptFrames").get<int>(), 1);
    EXPECT_TRUE(hasNote(l, "corrupt-frames"));
    const json& events = l.at("events");
    const auto gap = std::ranges::find_if(events, [](const json& e) { return e.at("type") == "gap"; });
    ASSERT_NE(gap, events.end());
    EXPECT_EQ(gap->at("reason"), "corrupt");
    EXPECT_NEAR(gap->at("t").get<double>(), 0.63, 1e-9);
    EXPECT_NEAR(gap->at("durationS").get<double>(), 0.07, 1e-9);
    // Every frame after the damage is back, starting at the next I frame.
    EXPECT_EQ(l.at("series").at("t").size(), 128u);
}

TEST(BlackboxDecoder, MissingFramesWithoutDamageAreAGap) {
    SyntheticLog log;
    log.fly(0.32);
    log.fly(0.32, 1000, 3'000'000);  // two seconds later
    log.end();
    const json l = decodeOne(log.bytes());
    const auto gap = std::ranges::find_if(l.at("events"), [](const json& e) { return e.at("type") == "gap"; });
    ASSERT_NE(gap, l.at("events").end());
    EXPECT_EQ(gap->at("reason"), "missing");
}

TEST(BlackboxDecoder, SeveralLogsInOneFile) {
    SyntheticLog first;
    first.fly(0.1);
    first.end();
    SyntheticLog second;
    second.end();
    std::vector<std::uint8_t> file = first.bytes();
    file.insert(file.end(), second.bytes().begin(), second.bytes().end());
    const json all = decodeAll(file);
    ASSERT_EQ(all.at("logs").size(), 2u);
    EXPECT_EQ(all.at("logs").at(0).at("index"), 1);
    EXPECT_EQ(all.at("logs").at(0).at("status"), "ok");
    EXPECT_EQ(all.at("logs").at(1).at("index"), 2);
    EXPECT_EQ(all.at("logs").at(1).at("status"), "header-only");
    EXPECT_EQ(all.at("logs").at(1).at("byteRange").at(0), first.bytes().size());
}

TEST(BlackboxDecoder, TruncatedLogHasNoEndMarker) {
    SyntheticLog log;
    log.fly(0.5);
    std::vector<std::uint8_t> bytes = log.bytes();
    bytes.resize(bytes.size() - 3);  // cut inside the last frame
    const json l = decodeOne(bytes);
    EXPECT_TRUE(hasNote(l, "log-truncated"));
    EXPECT_EQ(l.at("series").at("t").size(), 49u);
}

TEST(BlackboxDecoder, TimeRollsOverAfter71Minutes) {
    SyntheticLog log;
    log.fly(0.64, 0, 0xFFFFFFFFu - 200'000);
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "ok");
    EXPECT_EQ(l.at("series").at("t").size(), 64u);
    EXPECT_NEAR(l.at("durationS").get<double>(), 0.63, 1e-9);
}

TEST(BlackboxDecoder, LoggingResumeIsAPauseNotDamage) {
    SyntheticLog log;
    log.fly(0.32);
    log.loggingResume(5000, 60'000'000);
    log.fly(0.32, 5000, 60'000'000);
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "ok");
    const auto paused = std::ranges::find_if(l.at("events"), [](const json& e) { return e.at("type") == "logging-paused"; });
    ASSERT_NE(paused, l.at("events").end());
    EXPECT_NEAR(paused->at("durationS").get<double>(), 58.69, 1e-6);
    EXPECT_EQ(l.at("series").at("t").size(), 64u);
}

TEST(BlackboxDecoder, GpsWithoutFixAndPartialFix) {
    SyntheticLog noFix;
    noFix.slow(1, 0, 0);
    noFix.fly(0.1);
    noFix.gps({3, 0, 0, 0, 0, 0});
    noFix.gps({4, 485001652, 352007737, 0, 0, 0});
    noFix.end();
    const json a = decodeOne(noFix.bytes());
    EXPECT_TRUE(hasNote(a, "gps-no-fix"));
    EXPECT_FALSE(a.at("channels").at("gps").get<bool>());
    EXPECT_TRUE(a.at("gps").at("lat").at(0).is_null());

    SyntheticLog partial;
    partial.slow(1, 0b111, 0);
    partial.fly(0.1);
    partial.gps({12, 485001652, 352007737, 0, 0, 0});
    partial.slow(1, 0b101, 0);  // fix lost
    partial.fly(0.1, 10, 1'100'000);
    partial.gps({4, 485001700, 352007800, 0, 0, 0});
    partial.end();
    const json b = decodeOne(partial.bytes());
    EXPECT_TRUE(hasNote(b, "gps-fix-lost"));
    EXPECT_TRUE(b.at("channels").at("gps").get<bool>());
    EXPECT_FALSE(b.at("gps").at("fix").at(1).get<bool>());
}

TEST(BlackboxDecoder, NoCurrentSensorHidesCurrent) {
    SyntheticLog log("Betaflight 4.5.1 (77d01ba3b) STM32F7X2", {}, false);
    log.fly(0.1);
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_TRUE(hasNote(l, "no-current-sensor"));
    EXPECT_FALSE(l.at("channels").at("current").get<bool>());
    EXPECT_FALSE(l.at("series").contains("currentA"));
    EXPECT_TRUE(l.at("series").contains("voltageV"));
}

TEST(BlackboxDecoder, CurrentSensorReadingZero) {
    SyntheticLog log;
    MainValues v;
    v.amperage = 0;
    log.intra(v);
    v.iteration = 1;
    v.timeUs = 10'000;
    log.inter(v);
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_TRUE(hasNote(l, "current-reads-zero"));
    EXPECT_FALSE(l.at("channels").at("current").get<bool>());
}

TEST(BlackboxDecoder, CrsfLinkQualityFromDebugOrANote) {
    SyntheticLog plain("Betaflight 4.5.1 (77d01ba3b) STM32F7X2", {{"serialrx_provider", "9"}, {"debug_mode", "0"}});
    plain.fly(0.1);
    plain.end();
    const json a = decodeOne(plain.bytes());
    EXPECT_TRUE(hasNote(a, "link-quality-missing"));
    EXPECT_EQ(a.at("radio").at("protocol"), "CRSF");
    EXPECT_FALSE(a.at("channels").at("linkQuality").get<bool>());

    SyntheticLog withLq("Betaflight 4.5.1 (77d01ba3b) STM32F7X2", {{"serialrx_provider", "9"}, {"debug_mode", "52"}});
    MainValues v;
    v.debug0 = 87;  // uplink RSSI, −dBm
    v.debug2 = 99;  // link quality, %
    withLq.intra(v);
    withLq.end();
    const json b = decodeOne(withLq.bytes());
    EXPECT_FALSE(hasNote(b, "link-quality-missing"));
    EXPECT_EQ(b.at("series").at("linkQualityPct").at(0), 99.0);
    EXPECT_EQ(b.at("series").at("rssiDbm").at(0), -87.0);
}

TEST(BlackboxDecoder, OtherFirmwareGetsTheTrackOnly) {
    SyntheticLog log("INAV 7.1.0 (a1b2c3d) MATEKF405");
    log.fly(0.1);
    log.gps({12, 485001652, 352007737, 1192, 300, 0});
    log.end();
    const json l = decodeOne(log.bytes());
    EXPECT_EQ(l.at("status"), "partial");
    EXPECT_FALSE(l.at("firmware").at("supported").get<bool>());
    EXPECT_TRUE(hasNote(l, "firmware-units-unknown"));
    EXPECT_FALSE(l.at("series").contains("voltageV"));
    EXPECT_DOUBLE_EQ(l.at("gps").at("lat").at(0).get<double>(), 48.5001652);
    EXPECT_FALSE(l.at("gps").contains("altM"));
}

TEST(BlackboxDecoder, UnknownEncodingStopsWithAReason) {
    LogWriter w;
    w.text(BlackboxFile::kProductLine);
    w.byte('\n');
    w.headerLine("Field I name", "loopIteration,time");
    w.headerLine("Field I signed", "0,0");
    w.headerLine("Field I predictor", "0,0");
    w.headerLine("Field I encoding", "1,5");
    w.headerLine("Field P predictor", "6,2");
    w.headerLine("Field P encoding", "9,0");
    w.headerLine("Firmware revision", "Betaflight 4.5.1 (x) T");
    w.text("I\x01\x02");
    const json l = decodeOne(w.bytes());
    EXPECT_EQ(l.at("status"), "unsupported");
    EXPECT_TRUE(hasNote(l, "unsupported-format"));
}

TEST(BlackboxDecoder, NotABlackboxFile) {
    const std::vector<std::uint8_t> bytes = {'h', 'e', 'l', 'l', 'o'};
    EXPECT_TRUE(decodeAll(bytes).at("logs").empty());
    EXPECT_FALSE(BlackboxDecoder("fpv.blackbox", "1").probe(bytes).at("match").get<bool>());
}

// Optional check against a real log: FPV_BLACKBOX_LOG=<file.bfl> FPV_BLACKBOX_EXPECT=<json>
// where the JSON gives {"I": frames, "P": frames, "G": frames, "durationS": s}.
TEST(BlackboxDecoder, RealLogFromTheEnvironment) {
    const char* path = std::getenv("FPV_BLACKBOX_LOG");
    const char* expect = std::getenv("FPV_BLACKBOX_EXPECT");
    if (path == nullptr || expect == nullptr) GTEST_SKIP() << "FPV_BLACKBOX_LOG and FPV_BLACKBOX_EXPECT are not set";
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json l = decodeOne(bytes, 50.0);
    const json e = json::parse(expect);
    const json& frames = l.at("stats").at("frames");
    EXPECT_EQ(frames.at("I"), e.at("I"));
    EXPECT_EQ(frames.at("G"), e.at("G"));
    EXPECT_NEAR(l.at("durationS").get<double>(), e.at("durationS").get<double>(), 1e-6);
}
