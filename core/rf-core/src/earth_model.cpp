#include "rfcore/earth_model.hpp"

#include "rfcore/constants.hpp"

#include <cmath>
#include <stdexcept>

namespace rfcore {

EarthModel::EarthModel(double effectiveEarthFactor) : k_(effectiveEarthFactor) {
    if (!(k_ > 0.0) || !std::isfinite(k_)) {
        // k <= 0 or infinite means ducting; the model does not apply and we refuse to guess.
        throw std::invalid_argument("EarthModel: effective earth factor must be positive and finite");
    }
}

Metres EarthModel::radiusOfCurvature(double latitudeDeg, double azimuthDeg) {
    using constants::kPi;
    const double a = constants::kWgs84SemiMajorAxis;
    const double f = constants::kWgs84Flattening;
    const double e2 = f * (2.0 - f);
    const double phi = latitudeDeg * kPi / 180.0;
    const double alpha = azimuthDeg * kPi / 180.0;
    const double sinPhi = std::sin(phi);
    const double w2 = 1.0 - e2 * sinPhi * sinPhi;
    const double w = std::sqrt(w2);
    const double meridian = a * (1.0 - e2) / (w2 * w);  // M
    const double primeVertical = a / w;                 // N
    const double cosA = std::cos(alpha);
    const double sinA = std::sin(alpha);
    return Metres(1.0 / (cosA * cosA / meridian + sinA * sinA / primeVertical));
}

Metres EarthModel::effectiveRadius(double latitudeDeg, double azimuthDeg) const {
    return radiusOfCurvature(latitudeDeg, azimuthDeg) * k_;
}

double EarthModel::factorFromRefractivityGradient(double dNdhPerKm, Metres earthRadius) {
    return 1.0 / (1.0 + earthRadius.value() * dNdhPerKm * 1e-9);
}

}  // namespace rfcore
