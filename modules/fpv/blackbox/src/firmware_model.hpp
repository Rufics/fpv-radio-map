#pragma once

// What the logged numbers mean for one firmware release: units, mode bits and enum names, each
// taken from the Betaflight sources of that release (see the comments at each value). Only
// Betaflight is mapped; other firmware that writes the same format gets its GPS position decoded
// and the rest named as unsupported.

#include "log_header.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpv::blackbox {

/// boxId_e of one release range, from tools/blackbox/mode_tables.py.
struct ModeTableRow {
    Version since;
    const char* ref;
    std::vector<std::string_view> names;
};

class ModeTable {
public:
    /// The row for this firmware: the newest release not newer than it. Null for anything but
    /// Betaflight ≥ 3.1.
    static const ModeTable* forFirmware(const FirmwareInfo& firmware);
    static const std::vector<ModeTable>& all();

    explicit ModeTable(const ModeTableRow& row) : row_(row) {}

    /// Names of the bits set in rcModeActivationMask (the slow frame's flightModeFlags and the
    /// FLIGHTMODE event). Bits past the table are named BOX<n>.
    [[nodiscard]] std::vector<std::string> active(std::uint32_t mask) const;
    [[nodiscard]] const Version& since() const { return row_.since; }
    [[nodiscard]] std::string_view ref() const { return row_.ref; }

private:
    const ModeTableRow& row_;
};

class FirmwareModel {
public:
    /// debug_mode CRSF_LINK_STATISTICS_UPLINK, index 52 in debugModeNames from 4.1 to master.
    static constexpr long long kDebugCrsfLinkStatisticsUplink = 52;
    /// SERIALRX_CRSF in rx.h, the same value in every release since 3.1.
    static constexpr long long kSerialRxCrsf = 9;

    explicit FirmwareModel(const LogHeader& header);

    [[nodiscard]] const FirmwareInfo& firmware() const { return firmware_; }
    /// The release's units are known (Betaflight ≥ 3.1).
    [[nodiscard]] bool unitsKnown() const { return unitsKnown_; }

    // Logged units per SI unit (the decoder divides by these, so 2359 / 100 stays 23.59).
    /// vbatLatest units per volt.
    [[nodiscard]] std::optional<double> voltageDivisor() const;
    /// amperageLatest units per ampere.
    [[nodiscard]] std::optional<double> currentDivisor() const;
    /// GPS_altitude units per metre.
    [[nodiscard]] std::optional<double> gpsAltitudeDivisor() const;
    /// baroAlt units per metre.
    [[nodiscard]] std::optional<double> baroDivisor() const;
    /// Throttle comes from setpoint[3] (mixer throttle × 1000, 4.0+) instead of rcCommand[3].
    [[nodiscard]] bool throttleFromMixer() const;
    /// rcCommand values are multiplied by 10 with blackbox_high_resolution.
    [[nodiscard]] double rcCommandScale() const { return highResolution_ ? 10.0 : 1.0; }

    [[nodiscard]] bool crsf() const { return serialRx_ == kSerialRxCrsf; }
    [[nodiscard]] std::string radioProtocol() const;
    /// debug[] carries CRSF link statistics: [0] uplink RSSI (−dBm), [2] link quality (%).
    [[nodiscard]] bool linkStatisticsInDebug() const;
    /// The release has the CRSF link statistics debug mode (4.1+).
    [[nodiscard]] bool linkStatisticsDebugAvailable() const;

    [[nodiscard]] const ModeTable* modes() const { return modes_; }

    /// failsafePhase_e (failsafe.h); values were only ever appended.
    static std::string failsafePhase(std::int64_t phase);
    /// flightLogDisarmReason_e (core.h, logged since 4.2); values were only ever appended.
    static std::string disarmReason(std::int64_t reason);

private:
    FirmwareInfo firmware_;
    bool unitsKnown_ = false;
    bool highResolution_ = false;
    long long serialRx_ = -1;
    long long debugMode_ = -1;
    const ModeTable* modes_ = nullptr;
};

}  // namespace fpv::blackbox
