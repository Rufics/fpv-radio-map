#pragma once

namespace rfcore {

/// Normalised Fresnel integrals C(x) = ∫0^x cos(πt²/2) dt and S(x) = ∫0^x sin(πt²/2) dt.
///
/// Evaluated to full double precision: a convergent power series for |x| <= 1.5 and the
/// modified Lentz continued fraction of the complementary error function above that, both
/// iterated until the next term is below machine epsilon. For large |x| the complements
/// 1/2 - C and 1/2 - S are returned directly from the continued fraction, so expressions
/// such as 1 - C - S do not lose digits to cancellation.
class FresnelIntegrals {
public:
    struct Values {
        double c;  ///< C(x)
        double s;  ///< S(x)
        /// 1/2 - C(|x|) and 1/2 - S(|x|), computed without subtraction for |x| > 1.5.
        double halfMinusC;
        double halfMinusS;
    };

    [[nodiscard]] static Values evaluate(double x);
};

}  // namespace rfcore
