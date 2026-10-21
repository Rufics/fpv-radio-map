#pragma once

// Builds Betaflight-style logs for the tests: the header of a 4.5.1 log (trimmed to the fields
// the decoder reads) and frames encoded with the firmware's predictors and encodings, so a test
// states flight values and checks what comes out of the decoder.

#include "decoder.hpp"
#include "log_writer.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fpv::blackbox::test {

struct MainValues {
    std::uint32_t iteration = 0;
    std::uint32_t timeUs = 0;
    std::int32_t vbat = 1680;     // 0.01 V
    std::int32_t amperage = 0;    // 0.01 A
    std::int32_t rssi = 1023;     // 0..1023
    std::int32_t throttle = 0;    // setpoint[3], 0..1000
    std::int32_t debug0 = 0;
    std::int32_t debug2 = 0;
};

struct GpsValues {
    std::uint32_t sats = 12;
    std::int32_t lat = 0;  // absolute, 1e-7 degrees
    std::int32_t lon = 0;
    std::int32_t altDm = 0;
    std::uint32_t speedCms = 0;
    std::uint32_t courseDd = 0;
};

class SyntheticLog {
public:
    static constexpr std::int32_t kVbatRef = 1680;

    explicit SyntheticLog(std::string firmware = "Betaflight 4.5.1 (77d01ba3b) STM32F7X2",
                          std::vector<std::pair<std::string, std::string>> extraHeader = {},
                          bool withCurrent = true)
        : withCurrent_(withCurrent) {
        w_.text(BlackboxFile::kProductLine);
        w_.byte('\n');
        w_.headerLine("Data version", "2");
        w_.headerLine("I interval", "32");
        w_.headerLine("P interval", "1");
        if (withCurrent_) {
            w_.headerLine("Field I name", "loopIteration,time,vbatLatest,amperageLatest,rssi,setpoint[3],debug[0],debug[2]");
            w_.headerLine("Field I signed", "0,0,0,1,0,1,1,1");
            w_.headerLine("Field I predictor", "0,0,9,0,0,0,0,0");
            w_.headerLine("Field I encoding", "1,1,3,0,1,0,0,0");
            w_.headerLine("Field P predictor", "6,2,1,1,1,1,1,1");
            w_.headerLine("Field P encoding", "9,0,6,6,6,6,6,6");
        } else {
            w_.headerLine("Field I name", "loopIteration,time,vbatLatest,rssi,setpoint[3],debug[0],debug[2]");
            w_.headerLine("Field I signed", "0,0,0,0,1,1,1");
            w_.headerLine("Field I predictor", "0,0,9,0,0,0,0");
            w_.headerLine("Field I encoding", "1,1,3,1,0,0,0");
            w_.headerLine("Field P predictor", "6,2,1,1,1,1,1");
            w_.headerLine("Field P encoding", "9,0,6,6,6,6,6");
        }
        w_.headerLine("Field H name", "GPS_home[0],GPS_home[1]");
        w_.headerLine("Field H signed", "1,1");
        w_.headerLine("Field H predictor", "0,0");
        w_.headerLine("Field H encoding", "0,0");
        w_.headerLine("Field G name", "time,GPS_numSat,GPS_coord[0],GPS_coord[1],GPS_altitude,GPS_speed,GPS_ground_course");
        w_.headerLine("Field G signed", "0,0,1,1,1,0,0");
        w_.headerLine("Field G predictor", "10,0,7,7,0,0,0");
        w_.headerLine("Field G encoding", "1,1,0,0,0,1,1");
        w_.headerLine("Field S name", "flightModeFlags,stateFlags,failsafePhase,rxSignalReceived,rxFlightChannelsValid");
        w_.headerLine("Field S signed", "0,0,0,0,0");
        w_.headerLine("Field S predictor", "0,0,0,0,0");
        w_.headerLine("Field S encoding", "1,1,7,7,7");
        w_.headerLine("Firmware type", "Cleanflight");
        w_.headerLine("Firmware revision", firmware);
        w_.headerLine("Log start datetime", "2024-09-22T19:43:32.234+00:00");
        w_.headerLine("Craft name", "TestQuad");
        w_.headerLine("vbatref", "1680");
        w_.headerLine("vbatcellvoltage", "330,350,420");
        for (const auto& [name, value] : extraHeader) w_.headerLine(name, value);
    }

    void intra(const MainValues& v) {
        w_.byte('I');
        w_.unsignedVB(v.iteration);
        w_.unsignedVB(v.timeUs);
        w_.unsignedVB(static_cast<std::uint32_t>(kVbatRef - v.vbat) & 0x3FFFu);
        if (withCurrent_) w_.signedVB(v.amperage);
        w_.unsignedVB(static_cast<std::uint32_t>(v.rssi));
        w_.signedVB(v.throttle);
        w_.signedVB(v.debug0);
        w_.signedVB(v.debug2);
        prev_ = v;
        prev2_ = v;
    }

    /// The iteration must be prev + 1 (P interval 1); time is predicted on a straight line.
    void inter(const MainValues& v) {
        w_.byte('P');
        const std::int64_t predicted = 2 * static_cast<std::int64_t>(prev_.timeUs) - prev2_.timeUs;
        w_.signedVB(static_cast<std::int32_t>(static_cast<std::int64_t>(v.timeUs) - predicted));
        std::vector<std::int32_t> deltas = {v.vbat - prev_.vbat};
        if (withCurrent_) deltas.push_back(v.amperage - prev_.amperage);
        deltas.push_back(v.rssi - prev_.rssi);
        deltas.push_back(v.throttle - prev_.throttle);
        deltas.push_back(v.debug0 - prev_.debug0);
        deltas.push_back(v.debug2 - prev_.debug2);
        w_.tag8_8SVB(deltas);
        prev2_ = prev_;
        prev_ = v;
    }

    void home(std::int32_t lat, std::int32_t lon) {
        w_.byte('H');
        w_.signedVB(lat);
        w_.signedVB(lon);
        home_ = {lat, lon};
    }

    void gps(const GpsValues& g) {
        w_.byte('G');
        w_.unsignedVB(0);  // time = last main frame time
        w_.unsignedVB(g.sats);
        w_.signedVB(g.lat - home_[0]);
        w_.signedVB(g.lon - home_[1]);
        w_.signedVB(g.altDm);
        w_.unsignedVB(g.speedCms);
        w_.unsignedVB(g.courseDd);
    }

    void slow(std::uint32_t modes, std::uint32_t stateFlags, std::int32_t failsafePhase, bool rxSignal = true, bool rxValid = true) {
        w_.byte('S');
        w_.unsignedVB(modes);
        w_.unsignedVB(stateFlags);
        const std::array<std::int32_t, 3> tail = {failsafePhase, rxSignal ? 1 : 0, rxValid ? 1 : 0};
        w_.tag2_3S32(tail);
    }

    void disarm(std::uint32_t reason) {
        w_.byte('E');
        w_.byte(15);
        w_.unsignedVB(reason);
    }
    void flightMode(std::uint32_t flags, std::uint32_t last) {
        w_.byte('E');
        w_.byte(30);
        w_.unsignedVB(flags);
        w_.unsignedVB(last);
    }
    void loggingResume(std::uint32_t iteration, std::uint32_t timeUs) {
        w_.byte('E');
        w_.byte(14);
        w_.unsignedVB(iteration);
        w_.unsignedVB(timeUs);
        prev_.iteration = iteration;
        prev_.timeUs = timeUs;
    }
    void end() {
        w_.byte('E');
        w_.byte(255);
        w_.text(std::string_view("End of log\0", 11));
    }
    void raw(std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) w_.byte(b);
    }

    /// A steady flight: `seconds` long at 100 Hz from 1 s after boot, an I frame every 32.
    void fly(double seconds, std::uint32_t startIteration = 0, std::uint32_t startUs = 1'000'000) {
        const auto frames = static_cast<std::uint32_t>(seconds * 100.0);
        for (std::uint32_t i = 0; i < frames; ++i) {
            MainValues v;
            v.iteration = startIteration + i;
            v.timeUs = startUs + i * 10'000;
            v.vbat = 1680 - static_cast<std::int32_t>(i / 10);
            v.amperage = 1000;
            v.rssi = 900;
            v.throttle = 500;
            if (i % 32 == 0) {
                intra(v);
            } else {
                inter(v);
            }
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return w_.bytes(); }

private:
    LogWriter w_;
    bool withCurrent_;
    MainValues prev_;
    MainValues prev2_;
    std::array<std::int32_t, 2> home_{0, 0};
};

}  // namespace fpv::blackbox::test
