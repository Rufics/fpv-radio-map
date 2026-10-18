#pragma once

#include <rfcore/units.hpp>

#include <optional>
#include <string>
#include <vector>

namespace rfext {

using rfcore::Metres;

/// Air at one height, as a forecast gives it.
struct AirSample {
    Metres heightMsl;
    double temperatureC{};
    double pressureHpa{};       ///< Total pressure.
    double relativeHumidity{};  ///< Percent, 0..100.
};

/// Radio refractivity, ITU-R P.453-14.
class Refractivity {
public:
    /// Saturation water vapour pressure over water, hPa, with the enhancement factor (eqs. 9, 10).
    [[nodiscard]] static double saturationVapourPressure(double temperatureC, double pressureHpa);
    /// N = 77.6 Pd/T + 72 e/T + 3.75e5 e/T²  (eqs. 2-4; T in K, Pd and e in hPa).
    [[nodiscard]] static double n(const AirSample& air);
    /// Vertical gradient between two samples, N-units per km. Uses only the two given levels:
    /// no interpolation is applied.
    [[nodiscard]] static double gradientPerKm(const AirSample& lower, const AirSample& upper);
};

/// The gradient that applies to a path, and where it came from.
struct RefractionEstimate {
    double gradientPerKm;
    double nSurface;
    Metres layerBottom;
    Metres layerTop;
    std::string description;
};

/// Picks the layer of a forecast column that a path lives in: from the surface sample to the
/// first level at or above the higher antenna. Returns nothing if the column does not reach it.
std::optional<RefractionEstimate> refractionForPath(const std::vector<AirSample>& column, Metres highestAntennaMsl);

}  // namespace rfext
