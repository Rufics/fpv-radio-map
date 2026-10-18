#pragma once

#include <rfcore/link_budget.hpp>

namespace rfext {

/// How far the margin can move within the terrain data's stated vertical error.
struct MarginBand {
    rfcore::Decibel nominal;
    rfcore::Decibel worst;  ///< interior samples raised by their error, ends lowered
    rfcore::Decibel best;   ///< interior samples lowered, ends raised
    enum class Verdict { Reliable, Marginal, Blocked, BlockedMayWork } verdict{};
};

std::string toString(MarginBand::Verdict v);

/// Propagates the DEM's stated error through the whole evaluation (geometry, diffraction,
/// free-space loss and every other term).
///
/// Every sample may move by its own stated error. The diffraction parameter ν of the Bullington
/// construction rises with every interior height and falls as either end rises, so its range is
/// reached at the corners: interior all up or all down, each end up or down (8 evaluations). The
/// knife-edge loss J(ν) is not monotone (it ripples for ν < 0), so the loss range is the exact
/// minimum and maximum of J over that ν interval, found from its turning points. Every other term
/// takes its extreme over the corners. The band therefore contains every outcome within the
/// stated error; the tests check that against random perturbations.
class UncertaintyAnalyzer {
public:
    explicit UncertaintyAnalyzer(const rfcore::LinkEvaluator& evaluator) : evaluator_(evaluator) {}

    [[nodiscard]] MarginBand analyze(const rfcore::TerrainProfile& profile, rfcore::Hertz frequency,
                                     const rfcore::TransmitterEnd& tx, const rfcore::ReceiverEnd& rx) const;

    struct Extremes {
        double min;
        double max;
    };
    /// Exact extremes of J(ν) on [lo, hi]: the ends and every turning point inside, each turning
    /// point located by golden-section search to the limit of double precision.
    [[nodiscard]] static Extremes knifeEdgeExtremes(double lo, double hi);

private:
    const rfcore::LinkEvaluator& evaluator_;
};

}  // namespace rfext
