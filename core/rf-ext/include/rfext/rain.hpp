#pragma once

#include <rfcore/link_budget.hpp>

namespace rfext {

/// Specific attenuation of rain, ITU-R P.838-3.
class RainAttenuation {
public:
    struct Coefficients {
        double kH, alphaH, kV, alphaV;
    };
    /// Eqs. (2)-(3) with Tables 1-4. Valid 1-1000 GHz; throws outside that range.
    [[nodiscard]] static Coefficients coefficients(rfcore::Hertz frequency);
    /// γ = k R^α in dB/km, with k and α combined for the path elevation θ and the polarisation
    /// tilt τ (eqs. 4-5). τ = 45° for circular polarisation, 0° horizontal, 90° vertical.
    [[nodiscard]] static double specificDbPerKm(rfcore::Hertz frequency, double rainRateMmPerHour,
                                                double elevationDeg, double tiltDeg);
};

/// Loss term for rain along the whole ray at one rain rate.
class RainContributor final : public rfcore::ILossContributor {
public:
    /// @param rainRateMmPerHour the rate the forecast gives for the hour
    /// @param tiltDeg polarisation tilt (45 for circular)
    RainContributor(double rainRateMmPerHour, double tiltDeg, std::string rateSource);

    [[nodiscard]] std::vector<rfcore::BudgetTerm> contribute(const rfcore::PathContext& context) const override;
    [[nodiscard]] std::vector<rfcore::UncertaintyNote> notes(const rfcore::PathContext& context) const override;

private:
    double rate_;
    double tilt_;
    std::string source_;
};

}  // namespace rfext
