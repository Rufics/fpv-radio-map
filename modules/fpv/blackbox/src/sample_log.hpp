#pragma once

// A short Betaflight 4.5.1 log with known values, written like the firmware writes one: an I
// frame, a P frame, a GPS home and a GPS frame, then the end marker. Used by the self-test.

#include "decoder.hpp"
#include "log_writer.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace fpv::blackbox {

class SampleLog {
public:
    static constexpr std::int32_t kVbatRef = 1680;
    static constexpr std::int32_t kHomeLat = 500000000;  // 50.0°
    static constexpr std::int32_t kHomeLon = 300000000;  // 30.0°

    static std::vector<std::uint8_t> build() {
        LogWriter w;
        w.text(BlackboxFile::kProductLine);
        w.byte('\n');
        w.headerLine("Data version", "2");
        w.headerLine("I interval", "4");
        w.headerLine("P interval", "1");
        w.headerLine("Field I name", "loopIteration,time,vbatLatest,rssi");
        w.headerLine("Field I signed", "0,0,0,0");
        w.headerLine("Field I predictor", "0,0,9,0");
        w.headerLine("Field I encoding", "1,1,3,1");
        w.headerLine("Field P predictor", "6,2,1,1");
        w.headerLine("Field P encoding", "9,0,0,0");
        w.headerLine("Field H name", "GPS_home[0],GPS_home[1]");
        w.headerLine("Field H signed", "1,1");
        w.headerLine("Field H predictor", "0,0");
        w.headerLine("Field H encoding", "0,0");
        w.headerLine("Field G name", "GPS_numSat,GPS_coord[0],GPS_coord[1],GPS_altitude,GPS_speed,GPS_ground_course");
        w.headerLine("Field G signed", "0,1,1,1,0,0");
        w.headerLine("Field G predictor", "0,7,7,0,0,0");
        w.headerLine("Field G encoding", "1,0,0,0,1,1");
        w.headerLine("Firmware revision", "Betaflight 4.5.1 (77d01ba3b) STM32F7X2");
        w.headerLine("vbatref", "1680");

        // I frame: iteration 0, 1.000000 s, 16.00 V (NEG_14BIT of vbatref - vbat), RSSI 1023.
        w.byte('I');
        w.unsignedVB(0);
        w.unsignedVB(1'000'000);
        w.unsignedVB(static_cast<std::uint32_t>(kVbatRef - 1600) & 0x3FFFu);
        w.unsignedVB(1023);
        // P frame: iteration +1 (INC), time 1.001 s (2·prev − prev2 + 1000), vbat −2, RSSI same.
        w.byte('P');
        w.signedVB(1000);
        w.signedVB(-2);
        w.signedVB(0);
        // Home 50.0, 30.0; then a fix 100 and 200 units (1e-7°) away, 123.4 m, 10 m/s, 90°.
        w.byte('H');
        w.signedVB(kHomeLat);
        w.signedVB(kHomeLon);
        w.byte('G');
        w.unsignedVB(12);
        w.signedVB(100);
        w.signedVB(200);
        w.signedVB(1234);
        w.unsignedVB(1000);
        w.unsignedVB(900);
        w.byte('E');
        w.byte(255);
        w.text(std::string_view("End of log\0", 11));
        return w.bytes();
    }
};

}  // namespace fpv::blackbox
