#pragma once

// What happened in the flight, in words: the flight-mode bands under the timeline, the headline
// numbers, and the list of moments a pilot cares about (arming, mode changes, weak link, low
// cells, failsafe, GPS loss, damaged data, the disarm and why), each placed on the map.

#include "rfflightlog/flight_log.hpp"
#include "rfflightlog/timeline.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rfflightlog {

/// One stretch of the flight in one mode, as a pilot names it.
struct ModeBand {
    double from = 0.0;
    double to = 0.0;
    std::string band;  ///< DISARMED, ACRO, ANGLE, HORIZON, ALTHOLD, POSHOLD, GPS_RESCUE, FAILSAFE
};

class ModeBandBuilder {
public:
    [[nodiscard]] std::vector<ModeBand> build(const FlightLog& log) const;
    /// The band of one state: disarmed and failsafe first, then the strongest assist mode.
    [[nodiscard]] static std::string bandOf(bool armed, const std::vector<std::string>& modes, std::string_view failsafe);
    [[nodiscard]] static json toJson(const std::vector<ModeBand>& bands);
};

struct FlightSummary {
    double durationS = 0.0;
    double armedS = 0.0;
    int cells = 0;
    std::optional<double> distanceM;
    std::optional<double> maxHomeDistM;
    std::optional<double> maxAltM;
    std::optional<double> maxSpeedMps;
    std::optional<double> maxCurrentA;
    std::optional<double> usedMah;
    std::optional<double> usedWh;
    std::optional<double> startVoltageV;
    std::optional<double> minVoltageV;
    std::optional<double> minCellV;
    std::optional<double> minLinkPct;
    std::string linkChannel;  ///< linkQualityPct or rssiPct, the one minLinkPct is from
    std::optional<std::string> disarmReason;
    [[nodiscard]] json toJson() const;
};

class SummaryBuilder {
public:
    [[nodiscard]] FlightSummary build(const FlightLog& log, const Timeline& timeline,
                                      const std::vector<ModeBand>& bands) const;
    /// The channel that tells how good the radio link was: link quality when logged, else RSSI.
    [[nodiscard]] static std::string linkChannel(const Timeline& timeline);
};

struct StoryEvent {
    double t = 0.0;
    std::string type;
    std::string severity;  ///< info, warn or blocked
    std::string title;
    std::string detail;
    std::optional<double> lat;
    std::optional<double> lon;
    [[nodiscard]] json toJson() const;
};

struct StoryOptions {
    double lowLinkPct = 30.0;      ///< link below this for minLowS is a weak-link moment
    double minLowS = 1.0;
    std::optional<double> lowCellV;  ///< default: the log's warning cell voltage, else 3.5 V
    double minLowCellS = 2.0;
};

class StoryBuilder {
public:
    explicit StoryBuilder(const StoryOptions& options) : options_(options) {}
    [[nodiscard]] std::vector<StoryEvent> build(const FlightLog& log, const Timeline& timeline,
                                                const std::vector<ModeBand>& bands) const;
    [[nodiscard]] static std::string disarmText(const std::string& reason);
    [[nodiscard]] static std::string bandTitle(const std::string& band);

private:
    void addStates(const FlightLog& log, std::vector<StoryEvent>& out) const;
    void addBands(const std::vector<ModeBand>& bands, std::vector<StoryEvent>& out) const;
    void addLowLink(const Timeline& timeline, std::vector<StoryEvent>& out) const;
    void addLowCell(const FlightLog& log, const Timeline& timeline, std::vector<StoryEvent>& out) const;
    void addGpsLoss(const Timeline& timeline, std::vector<StoryEvent>& out) const;
    void addRecords(const Timeline& timeline, std::vector<StoryEvent>& out) const;
    void addLogEvents(const FlightLog& log, std::vector<StoryEvent>& out) const;
    static void place(const Timeline& timeline, std::vector<StoryEvent>& events);
    StoryOptions options_;
};

}  // namespace rfflightlog
