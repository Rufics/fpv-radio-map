#pragma once

// The one interface the server and the map use for any flight log: a decoder (fpv.blackbox
// today, a DJI decoder later) answers the normalized flightlog/v1 document, and this turns each
// flight in it into the replay view (flightview/v1) the UI draws and the database stores.

#include "rfflightlog/flight_log.hpp"
#include "rfflightlog/plan_comparison.hpp"
#include "rfflightlog/story.hpp"
#include "rfflightlog/timeline.hpp"

#include <vector>

namespace rfflightlog {

struct ProcessOptions {
    double timelineHz = 10.0;
    StoryOptions story;
};

/// One flight, processed.
struct ProcessedFlight {
    FlightLog log;
    Timeline timeline;
    std::vector<ModeBand> bands;
    FlightSummary summary;
    std::vector<StoryEvent> story;

    /// The whole replay view (flightview/v1).
    [[nodiscard]] json toJson() const;
    /// The few fields a logbook row shows, without the samples.
    [[nodiscard]] json summaryJson() const;
};

class IFlightLogProcessor {
public:
    virtual ~IFlightLogProcessor() = default;
    /// Every flight of a normalized document; header-only and unsupported logs are kept, with
    /// their notes and no samples, so the UI can say why there is nothing to replay.
    [[nodiscard]] virtual std::vector<ProcessedFlight> process(const json& normalized, const ProcessOptions& options) const = 0;
    [[nodiscard]] virtual json compare(const ProcessedFlight& flight, const RoutePlan& plan) const = 0;
};

class FlightLogProcessor final : public IFlightLogProcessor {
public:
    [[nodiscard]] std::vector<ProcessedFlight> process(const json& normalized, const ProcessOptions& options) const override;
    [[nodiscard]] json compare(const ProcessedFlight& flight, const RoutePlan& plan) const override;
    [[nodiscard]] static ProcessedFlight processOne(const json& entry, const ProcessOptions& options);
};

}  // namespace rfflightlog
