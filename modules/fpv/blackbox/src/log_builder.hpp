#pragma once

// Turns the decoded frames of one Betaflight log into one entry of the normalized flight log
// (schemas/flightlog.schema.json): SI units, seconds from the first main frame, the main loop
// thinned to the requested sample rate, every GPS solution, state changes, events and notes.

#include "firmware_model.hpp"
#include "frame_parser.hpp"
#include "log_header.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fpv::blackbox {

using json = nlohmann::json;

struct BuildOptions {
    double sampleHz = 50.0;           ///< main loop samples kept per second
    std::size_t maxSamples = 360000;  ///< two hours at 50 Hz; later samples are dropped with a note
};

/// One series of the normalized log; NaN is a sample without a value (null in JSON).
class Column {
public:
    void push(std::optional<double> v);
    [[nodiscard]] json toJson() const;
    [[nodiscard]] bool anyValue() const { return values_ > 0; }
    [[nodiscard]] double maxAbs() const { return maxAbs_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    std::vector<double> data_;
    std::size_t values_ = 0;
    double maxAbs_ = 0.0;
};

class NormalizedLogBuilder final : public IFrameSink {
public:
    NormalizedLogBuilder(const LogHeader& header, BuildOptions options);

    void onMainFrame(std::span<const std::int64_t> values, std::int64_t timeUs) override;
    void onGpsFrame(std::span<const std::int64_t> values, std::int64_t timeUs) override;
    void onHomeFrame(std::span<const std::int64_t> values, std::int64_t timeUs) override;
    void onSlowFrame(std::span<const std::int64_t> values, std::int64_t timeUs) override;
    void onEvent(const LogEvent& event) override;
    void onGap(std::int64_t fromUs, std::int64_t toUs, GapCause cause) override;

    /// The finished log entry. `index` counts from 1 in the file.
    [[nodiscard]] json finish(const ParseStats& stats, int index, std::pair<std::size_t, std::size_t> byteRange);

private:
    struct MainFields {
        std::optional<std::size_t> vbat, amperage, baro, rssi, throttleMixer, throttleRc, debug0, debug2;
    };
    struct GpsFields {
        std::optional<std::size_t> sats, lat, lon, alt, speed, course;
    };
    struct SlowFields {
        std::optional<std::size_t> modes, state, failsafe, rxSignal, rxValid;
    };
    struct State {
        std::int64_t timeUs = 0;
        std::optional<bool> armed;
        std::optional<std::vector<std::string>> modes;
        std::optional<std::string> failsafe;
        std::optional<bool> rxSignal, rxValid, gpsFix, gpsHomeSet;
        [[nodiscard]] bool sameAs(const State& o) const {
            return armed == o.armed && modes == o.modes && failsafe == o.failsafe && rxSignal == o.rxSignal &&
                   rxValid == o.rxValid && gpsFix == o.gpsFix && gpsHomeSet == o.gpsHomeSet;
        }
    };
    struct Event {
        std::int64_t timeUs = 0;
        json body;
    };
    struct GpsSample {
        std::int64_t timeUs = 0;
        std::optional<double> lat, lon, alt, speed, course, sats;
        bool fix = false;
    };

    static std::optional<double> field(std::span<const std::int64_t> values, const std::optional<std::size_t>& index);
    static std::optional<double> divided(std::optional<double> raw, const std::optional<double>& divisor);
    [[nodiscard]] std::optional<double> throttlePct(std::span<const std::int64_t> values) const;
    void applyModes(State& s, std::uint32_t mask) const;
    void pushState(State s);

    [[nodiscard]] double seconds(std::int64_t timeUs) const;
    [[nodiscard]] json firmwareJson() const;
    [[nodiscard]] json channelsJson() const;
    [[nodiscard]] json seriesJson() const;
    [[nodiscard]] json gpsJson() const;
    [[nodiscard]] json statesJson() const;
    [[nodiscard]] json eventsJson() const;
    [[nodiscard]] json homesJson() const;
    [[nodiscard]] json statsJson(const ParseStats& stats) const;
    [[nodiscard]] json notesJson(const ParseStats& stats) const;
    void addSensorNotes(json& notes) const;
    void addGpsNotes(json& notes) const;
    void addLinkNotes(json& notes) const;
    [[nodiscard]] std::optional<std::string> startedAt() const;
    [[nodiscard]] std::string status(const ParseStats& stats) const;

    const LogHeader& header_;
    FirmwareModel model_;
    BuildOptions options_;
    std::int64_t intervalUs_;
    MainFields main_;
    GpsFields gpsFields_;
    SlowFields slowFields_;

    std::optional<std::int64_t> originUs_;
    std::optional<std::int64_t> lastKeptUs_;
    bool keepNext_ = false;
    bool samplesTruncated_ = false;
    std::vector<std::int64_t> timeUs_;
    Column voltage_, current_, baro_, rssi_, linkQuality_, rssiDbm_, throttle_;

    std::vector<GpsSample> gps_;
    std::vector<std::pair<std::int64_t, json>> homes_;
    std::vector<State> states_;
    State state_;
    std::vector<Event> events_;
    double lostS_ = 0.0;
    std::size_t gaps_ = 0;
};

}  // namespace fpv::blackbox
