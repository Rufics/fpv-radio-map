#pragma once

// The flight on one even time base, ready for a chart or a map scrubber: every source channel
// resampled to the same instants, plus what is derived from them (distance from home, distance
// flown, climb rate, per-cell voltage, energy used).

#include "rfflightlog/flight_log.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace rfflightlog {

/// Channels on an even time base `t = start + i / hz`.
class Timeline {
public:
    double hz = 10.0;
    SampledChannels samples;

    [[nodiscard]] std::size_t size() const { return samples.t.size(); }
    /// The value of `name` at sample `i`, or kNoValue when the channel or the value is missing.
    [[nodiscard]] double at(const std::string& name, std::size_t i) const;
    /// The sample nearest to `t`, clamped to the ends.
    [[nodiscard]] std::size_t indexAt(double t) const;
    [[nodiscard]] json toJson() const;
};

/// Linear interpolation of an irregular series onto given instants. Two neighbours further
/// apart than `maxGapS`, or either of them missing, give no value: a gap stays a gap.
class Resampler {
public:
    explicit Resampler(double maxGapS) : maxGapS_(maxGapS) {}
    [[nodiscard]] std::vector<double> resample(const std::vector<double>& t, const std::vector<double>& v,
                                               const std::vector<double>& at) const;

private:
    [[nodiscard]] double between(const std::vector<double>& t, const std::vector<double>& v, std::size_t i,
                                 double x) const;
    double maxGapS_;
};

/// Current and voltage integrated over time (trapezoids); a gap in either stops the sum
/// growing instead of guessing.
class EnergyMeter {
public:
    struct Totals {
        std::vector<double> usedMah;
        std::vector<double> usedWh;
    };
    explicit EnergyMeter(double maxGapS) : maxGapS_(maxGapS) {}
    [[nodiscard]] Totals integrate(const std::vector<double>& t, const std::vector<double>& voltageV,
                                   const std::vector<double>& currentA) const;

private:
    double maxGapS_;
};

/// Series cells from the pack voltage at the start: below one full cell per cell.
class CellCounter {
public:
    explicit CellCounter(double cellMaxV) : cellMaxV_(cellMaxV) {}
    [[nodiscard]] int count(const std::vector<double>& t, const std::vector<double>& voltageV) const;

private:
    double cellMaxV_;
};

class TimelineBuilder {
public:
    struct Options {
        double hz = 10.0;
        double maxGapS = 1.0;      ///< telemetry gap that is not bridged
        double maxGpsGapS = 2.5;   ///< GPS updates slower, so its gaps may be longer
    };

    explicit TimelineBuilder(Options options) : options_(options) {}
    [[nodiscard]] Timeline build(const FlightLog& log) const;
    /// Cells found while building (0 without a voltage).
    [[nodiscard]] static int cells(const FlightLog& log);

private:
    [[nodiscard]] std::vector<double> instants(const FlightLog& log) const;
    void addSeries(const FlightLog& log, Timeline& out) const;
    void addEnergy(const FlightLog& log, Timeline& out) const;
    void addGps(const FlightLog& log, Timeline& out) const;
    static void addArmed(const FlightLog& log, Timeline& out);
    static void addAltitude(const FlightLog& log, Timeline& out);
    static void addClimb(Timeline& out);
    Options options_;
};

}  // namespace rfflightlog
