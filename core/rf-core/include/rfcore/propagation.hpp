#pragma once

#include "rfcore/units.hpp"

namespace rfcore {

/// Wavelength λ = c / f.
[[nodiscard]] Metres wavelength(Hertz frequency);

/// Free-space basic transmission loss, L = 20 log10(4π d f / c) (ITU-R P.525).
class FreeSpaceLoss {
public:
    [[nodiscard]] static Decibel compute(Metres distance, Hertz frequency);
};

/// Fresnel zones around the direct ray.
class FresnelZone {
public:
    /// Radius of the n-th zone at a point d1 from one end and d2 from the other:
    /// r_n = sqrt(n λ d1 d2 / (d1 + d2)).
    [[nodiscard]] static Metres radius(int zone, Metres wavelength, Metres d1, Metres d2);
};

/// Single knife-edge diffraction (ITU-R P.526, §4.1).
class KnifeEdge {
public:
    /// Fresnel-Kirchhoff parameter ν = h sqrt(2 (d1 + d2) / (λ d1 d2)).
    /// h is the height of the edge above the direct ray (negative when the ray clears it).
    [[nodiscard]] static double nu(Metres h, Metres wavelength, Metres d1, Metres d2);

    /// Diffraction loss J(ν) = -20 log10( sqrt((1 - C - S)² + (C - S)²) / 2 ), the exact form
    /// of ITU-R P.526 eq. (26). The approximation of eq. (31) is not used.
    [[nodiscard]] static Decibel loss(double nu);
};

}  // namespace rfcore
