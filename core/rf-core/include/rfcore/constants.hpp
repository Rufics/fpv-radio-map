#pragma once

#include "rfcore/units.hpp"

#include <numbers>

namespace rfcore::constants {

/// Speed of light in vacuum, exact by the SI definition of the metre.
inline constexpr double kSpeedOfLight = 299'792'458.0;  // m/s
/// Boltzmann constant, exact since the 2019 SI redefinition.
inline constexpr double kBoltzmann = 1.380649e-23;  // J/K
/// Reference noise temperature used for noise figures (IEEE / ITU-R convention).
inline constexpr Kelvin kReferenceNoiseTemperature{290.0};

/// WGS84 defining constants (NIMA TR8350.2).
inline constexpr double kWgs84SemiMajorAxis = 6'378'137.0;     // m
inline constexpr double kWgs84Flattening = 1.0 / 298.257223563;

/// Effective-earth factor for the ITU-R standard atmosphere (refractivity gradient
/// -40 N-units/km, ITU-R P.453). Used only when no forecast is available, and the
/// result says so.
inline constexpr double kStandardEffectiveEarthFactor = 4.0 / 3.0;

inline constexpr double kPi = std::numbers::pi;

}  // namespace rfcore::constants
