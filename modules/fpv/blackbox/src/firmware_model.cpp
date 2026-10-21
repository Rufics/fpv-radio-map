#include "firmware_model.hpp"

#include <array>
#include <format>

namespace fpv::blackbox {
namespace {

const std::vector<ModeTableRow>& rows() {
    static const std::vector<ModeTableRow> table = {
#include "mode_tables.inc"
    };
    return table;
}

constexpr Version kV3_1{3, 1, 0};
constexpr Version kV3_4{3, 4, 0};
constexpr Version kV4_0{4, 0, 0};
constexpr Version kV4_1{4, 1, 0};
constexpr Version kV4_3{4, 3, 0};

}  // namespace

const std::vector<ModeTable>& ModeTable::all() {
    static const std::vector<ModeTable> tables = [] {
        std::vector<ModeTable> out;
        for (const ModeTableRow& row : rows()) out.emplace_back(row);
        return out;
    }();
    return tables;
}

const ModeTable* ModeTable::forFirmware(const FirmwareInfo& firmware) {
    if (!firmware.betaflightAtLeast(kV3_1)) return nullptr;
    const ModeTable* chosen = nullptr;
    for (const ModeTable& table : all()) {
        if (table.since() <= *firmware.version()) chosen = &table;
    }
    return chosen;
}

std::vector<std::string> ModeTable::active(std::uint32_t mask) const {
    std::vector<std::string> names;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (((mask >> bit) & 1u) == 0) continue;
        names.push_back(bit < row_.names.size() ? std::string(row_.names[bit]) : std::format("BOX{}", bit));
    }
    return names;
}

FirmwareModel::FirmwareModel(const LogHeader& header) : firmware_(header.firmware()) {
    unitsKnown_ = firmware_.betaflightAtLeast(kV3_1);
    highResolution_ = header.integer("blackbox_high_resolution").value_or(0) != 0;
    serialRx_ = header.integer("serialrx_provider").value_or(-1);
    debugMode_ = header.integer("debug_mode").value_or(-1);
    modes_ = ModeTable::forFirmware(firmware_);
}

std::optional<double> FirmwareModel::voltageDivisor() const {
    if (!unitsKnown_) return std::nullopt;
    // 4.0 moved the voltage meter to 0.01 V (getBatteryVoltageLatest); 3.1 to 3.5 logged 0.1 V.
    return firmware_.betaflightAtLeast(kV4_0) ? 100.0 : 10.0;
}

std::optional<double> FirmwareModel::currentDivisor() const {
    if (!unitsKnown_) return std::nullopt;
    return 100.0;  // amperageLatest is in centiamperes since 3.1
}

std::optional<double> FirmwareModel::gpsAltitudeDivisor() const {
    if (!unitsKnown_) return std::nullopt;
    // 3.1 to 3.3 logged gpsSol.llh.alt in metres, 3.4 and 3.5 in centimetres (ublox altitude_msl
    // / 10, NMEA * 10), 4.0 on gpsSol.llh.altCm / 10, decimetres.
    if (firmware_.betaflightAtLeast(kV4_0)) return 10.0;
    if (firmware_.betaflightAtLeast(kV3_4)) return 100.0;
    return 1.0;
}

std::optional<double> FirmwareModel::baroDivisor() const {
    if (!unitsKnown_) return std::nullopt;
    return 100.0;  // baro altitude in centimetres
}

bool FirmwareModel::throttleFromMixer() const { return firmware_.betaflightAtLeast(kV4_0); }

std::string FirmwareModel::radioProtocol() const {
    // rx.h serialRxType_e: 1 to 12 are the same in every release since 3.1.
    static constexpr std::array<const char*, 13> kStable = {
        "", "SPEKTRUM2048", "SBUS", "SUMD", "SUMH", "XBUS_MODE_B", "XBUS_MODE_B_RJ01", "IBUS", "JETIEXBUS", "CRSF",
        "SRXL", "TARGET_CUSTOM", "FPORT"};
    if (serialRx_ >= 1 && serialRx_ < static_cast<long long>(kStable.size())) return kStable[static_cast<std::size_t>(serialRx_)];
    if (serialRx_ == 13 && firmware_.betaflightAtLeast(kV4_1)) return "SRXL2";
    if (serialRx_ == 14 && firmware_.betaflightAtLeast(kV4_3)) return "GHST";
    return {};
}

bool FirmwareModel::linkStatisticsDebugAvailable() const { return firmware_.betaflightAtLeast(kV4_1); }

bool FirmwareModel::linkStatisticsInDebug() const {
    return linkStatisticsDebugAvailable() && crsf() && debugMode_ == kDebugCrsfLinkStatisticsUplink;
}

std::string FirmwareModel::failsafePhase(std::int64_t phase) {
    static constexpr std::array<const char*, 8> kNames = {
        "IDLE", "RX_LOSS_DETECTED", "LANDING", "LANDED", "RX_LOSS_MONITORING", "RX_LOSS_RECOVERED", "GPS_RESCUE", "AUTOPILOT"};
    if (phase >= 0 && phase < static_cast<std::int64_t>(kNames.size())) return kNames[static_cast<std::size_t>(phase)];
    return std::format("PHASE_{}", phase);
}

std::string FirmwareModel::disarmReason(std::int64_t reason) {
    static constexpr std::array<const char*, 12> kNames = {
        "ARMING_DISABLED", "FAILSAFE",       "THROTTLE_TIMEOUT", "STICKS",  "SWITCH",   "CRASH_PROTECTION",
        "RUNAWAY_TAKEOFF", "GPS_RESCUE",     "SERIAL_COMMAND",   "LANDING", "CRASHFLIP", "LAUNCH_ABORT"};
    if (reason >= 0 && reason < static_cast<std::int64_t>(kNames.size())) return kNames[static_cast<std::size_t>(reason)];
    return std::format("REASON_{}", reason);
}

}  // namespace fpv::blackbox
