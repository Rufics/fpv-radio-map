#pragma once

#include "rfcore/path_geometry.hpp"
#include "rfcore/units.hpp"

#include <cstddef>
#include <optional>

namespace rfcore {

/// Result of the terrain diffraction calculation for one path.
struct DiffractionResult {
    bool lineOfSight;            ///< True when no profile point rises above the direct ray.
    double nu;                   ///< ν of the governing (real or equivalent) edge.
    Metres edgeHeight;           ///< Height of that edge above the direct ray (negative: clearance).
    Metres d1;                   ///< Along the ray from the first antenna to the edge.
    Metres d2;                   ///< Along the ray from the edge to the second antenna.
    std::size_t dominantSample;  ///< Index of the profile point that governs (from the first antenna side).
    Decibel loss;                ///< J(ν), exact form.

    /// Smallest clearance of the first Fresnel zone over the whole path, as a fraction of r1:
    /// 1.0 = fully clear, 0.6 = the usual planning rule, 0 = ray grazes, negative = blocked.
    double minFresnelClearance;
    std::size_t minClearanceSample;
    Metres firstFresnelRadiusAtMin;
};

/// Terrain diffraction by the Bullington construction (ITU-R P.526-15 §4.5) with exact
/// geometry and the exact knife-edge function.
///
/// Model notes, reported with every result:
///  * The path is reduced to one equivalent knife edge. This is the standard construction;
///    it is a physical model, so its error is a modelling error, not a numerical one.
///  * P.526 adds an empirical term [1 - exp(-Luc/6)](10 + 0.02 d) to Bullington loss for
///    general paths. It is off by default because it is a curve fit; enable it per mission
///    with `useEmpiricalCorrection`, and the result names it as its own loss term.
class BullingtonDiffraction {
public:
    struct Options {
        bool useEmpiricalCorrection = false;
    };

    explicit BullingtonDiffraction(Options options);
    BullingtonDiffraction();

    [[nodiscard]] DiffractionResult compute(const PathGeometry& geometry, Metres wavelength) const;

    /// The P.526 empirical correction for a computed knife-edge loss and path length.
    [[nodiscard]] static Decibel empiricalCorrection(Decibel knifeEdgeLoss, Metres pathLength);

    [[nodiscard]] const Options& options() const { return options_; }

private:
    Options options_;
};

}  // namespace rfcore
