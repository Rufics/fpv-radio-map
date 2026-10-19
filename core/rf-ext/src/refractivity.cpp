#include "rfext/refractivity.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace rfext {

double Refractivity::saturationVapourPressure(double t, double p) {
    // P.453-14 eq. (9)-(10), water: a = 6.1121, b = 18.678, c = 257.14, d = 234.5.
    const double ef = 1.0 + 1e-4 * (7.2 + p * (0.0320 + 5.9e-6 * t * t));
    return ef * 6.1121 * std::exp((18.678 - t / 234.5) * t / (t + 257.14));
}

double Refractivity::n(const AirSample& air) {
    if (!(air.relativeHumidity >= 0.0 && air.relativeHumidity <= 100.0) || !(air.pressureHpa > 0.0) ||
        !(air.temperatureC > -273.15)) {
        throw std::invalid_argument("Refractivity: air sample out of range");
    }
    const double tK = air.temperatureC + 273.15;
    const double e = air.relativeHumidity / 100.0 * saturationVapourPressure(air.temperatureC, air.pressureHpa);
    const double pd = air.pressureHpa - e;
    return 77.6 * pd / tK + 72.0 * e / tK + 3.75e5 * e / (tK * tK);
}

double Refractivity::gradientPerKm(const AirSample& lower, const AirSample& upper) {
    const double dh = (upper.heightMsl - lower.heightMsl).value();
    if (!(dh > 0.0)) throw std::invalid_argument("Refractivity: upper level must be above the lower one");
    return (n(upper) - n(lower)) / dh * 1000.0;
}

std::optional<RefractionEstimate> refractionForPath(const std::vector<AirSample>& column, Metres highestAntennaMsl) {
    if (column.size() < 2) return std::nullopt;
    std::vector<AirSample> sorted = column;
    std::ranges::sort(sorted,
                      [](const AirSample& a, const AirSample& b) { return a.heightMsl.value() < b.heightMsl.value(); });
    const AirSample& surface = sorted.front();
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].heightMsl.value() >= highestAntennaMsl.value() && sorted[i].heightMsl.value() > surface.heightMsl.value()) {
            RefractionEstimate r{Refractivity::gradientPerKm(surface, sorted[i]), Refractivity::n(surface),
                                 surface.heightMsl, sorted[i].heightMsl, {}};
            std::ostringstream d;
            d.precision(4);
            d << "dN/dh " << r.gradientPerKm << " N/km between " << surface.heightMsl.value() << " m and "
              << sorted[i].heightMsl.value() << " m MSL (ITU-R P.453-14)";
            r.description = d.str();
            return r;
        }
    }
    return std::nullopt;
}

}  // namespace rfext
