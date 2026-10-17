#include "rfcore/propagation.hpp"

#include "rfcore/constants.hpp"
#include "rfcore/fresnel_integrals.hpp"

#include <cmath>
#include <stdexcept>

namespace rfcore {

Metres wavelength(Hertz frequency) {
    if (!(frequency.value() > 0.0)) {
        throw std::invalid_argument("wavelength: frequency must be positive");
    }
    return Metres(constants::kSpeedOfLight / frequency.value());
}

Decibel FreeSpaceLoss::compute(Metres distance, Hertz frequency) {
    if (!(distance.value() > 0.0)) {
        throw std::invalid_argument("FreeSpaceLoss: distance must be positive");
    }
    if (!(frequency.value() > 0.0)) {
        throw std::invalid_argument("FreeSpaceLoss: frequency must be positive");
    }
    return Decibel(20.0 * std::log10(4.0 * constants::kPi * distance.value() * frequency.value() /
                                     constants::kSpeedOfLight));
}

Metres FresnelZone::radius(int zone, Metres lambda, Metres d1, Metres d2) {
    if (zone < 1) {
        throw std::invalid_argument("FresnelZone: zone number starts at 1");
    }
    if (!(d1.value() > 0.0) || !(d2.value() > 0.0)) {
        throw std::invalid_argument("FresnelZone: d1 and d2 must be positive");
    }
    return Metres(std::sqrt(zone * lambda.value() * d1.value() * d2.value() / (d1.value() + d2.value())));
}

double KnifeEdge::nu(Metres h, Metres lambda, Metres d1, Metres d2) {
    if (!(d1.value() > 0.0) || !(d2.value() > 0.0)) {
        throw std::invalid_argument("KnifeEdge: d1 and d2 must be positive");
    }
    return h.value() * std::sqrt(2.0 * (d1.value() + d2.value()) / (lambda.value() * d1.value() * d2.value()));
}

Decibel KnifeEdge::loss(double v) {
    const FresnelIntegrals::Values f = FresnelIntegrals::evaluate(v);
    double oneMinusCMinusS = 0.0;
    double cMinusS = 0.0;
    if (v >= 0.0) {
        // 1 - C - S and C - S from the complements: no cancellation for large ν.
        oneMinusCMinusS = f.halfMinusC + f.halfMinusS;
        cMinusS = f.halfMinusS - f.halfMinusC;
    } else {
        oneMinusCMinusS = 1.0 - f.c - f.s;
        cMinusS = f.c - f.s;
    }
    const double amplitude = std::hypot(oneMinusCMinusS, cMinusS) / 2.0;
    return Decibel(-20.0 * std::log10(amplitude));
}

}  // namespace rfcore
