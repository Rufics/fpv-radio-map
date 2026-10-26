#include "rfflightlog/processor.hpp"

#include <algorithm>
#include <stdexcept>

namespace rfflightlog {

namespace {

/// Samples a decoder thinned to a low rate are further apart than the default gap: allow three sample periods.
double gapFor(const json& entry, double defaultGapS) {
    const auto stats = entry.find("stats");
    if (stats == entry.end() || !stats->is_object()) return defaultGapS;
    const auto hz = stats->find("sampleHz");
    if (hz == stats->end() || !hz->is_number()) return defaultGapS;
    const double sampleHz = hz->get<double>();
    return sampleHz > 0.0 ? std::max(defaultGapS, 3.0 / sampleHz) : defaultGapS;
}

}  // namespace

json ProcessedFlight::summaryJson() const {
    json out = {{"index", log.index}, {"status", log.status}, {"durationS", log.durationS},
                {"firmware", log.firmware}, {"summary", summary.toJson()}};
    if (log.craft) out["craft"] = *log.craft;
    if (log.startedAt) out["startedAt"] = *log.startedAt;
    json bandsJson = ModeBandBuilder::toJson(bands);
    out["bands"] = std::move(bandsJson);
    json warnings = json::array();
    for (const StoryEvent& e : story) {
        if (e.severity != "info") warnings.push_back(e.title);
    }
    out["warnings"] = std::move(warnings);
    return out;
}

json ProcessedFlight::toJson() const {
    json out = summaryJson();
    out["schema"] = "flightview/v1";
    out["channels"] = log.channels;
    out["notes"] = log.notes;
    if (log.radioProtocol) out["radio"] = {{"protocol", *log.radioProtocol}};
    json homes = json::array();
    for (const HomePoint& h : log.homes) homes.push_back({{"t", h.t}, {"lat", h.lat}, {"lon", h.lon}});
    out["homes"] = std::move(homes);
    json events = json::array();
    for (const StoryEvent& e : story) events.push_back(e.toJson());
    out["story"] = std::move(events);
    out["timeline"] = timeline.toJson();
    return out;
}

ProcessedFlight FlightLogProcessor::processOne(const json& entry, const ProcessOptions& options) {
    ProcessedFlight f;
    f.log = FlightLog::fromJson(entry);
    TimelineBuilder::Options timelineOptions;
    timelineOptions.hz = options.timelineHz;
    timelineOptions.maxGapS = gapFor(entry, timelineOptions.maxGapS);
    f.timeline = TimelineBuilder(timelineOptions).build(f.log);
    f.bands = ModeBandBuilder().build(f.log);
    f.summary = SummaryBuilder().build(f.log, f.timeline, f.bands);
    f.story = StoryBuilder(options.story).build(f.log, f.timeline, f.bands);
    return f;
}

std::vector<ProcessedFlight> FlightLogProcessor::process(const json& normalized, const ProcessOptions& options) const {
    if (normalized.value("schema", "") != "flightlog/v1") throw std::invalid_argument("not a flightlog/v1 document");
    if (!(options.timelineHz > 0.0 && options.timelineHz <= 100.0)) throw std::invalid_argument("timelineHz must be in (0, 100]");
    std::vector<ProcessedFlight> out;
    for (const json& entry : normalized.at("logs")) out.push_back(processOne(entry, options));
    return out;
}

json FlightLogProcessor::compare(const ProcessedFlight& flight, const RoutePlan& plan) const {
    return PlanComparison(PlanComparison::Options{}).compare(plan, flight.timeline, flight.summary);
}

}  // namespace rfflightlog
