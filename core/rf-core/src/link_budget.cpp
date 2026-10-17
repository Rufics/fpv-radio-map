#include "rfcore/link_budget.hpp"

#include "rfcore/path_geometry.hpp"
#include "rfcore/propagation.hpp"

#include <algorithm>
#include <stdexcept>

namespace rfcore {

LinkEvaluator::LinkEvaluator(EarthModel earth, BullingtonDiffraction diffraction)
    : earth_(earth), diffraction_(diffraction) {}

void LinkEvaluator::addContributor(std::shared_ptr<const ILossContributor> contributor) {
    if (!contributor) {
        throw std::invalid_argument("LinkEvaluator: null contributor");
    }
    contributors_.push_back(std::move(contributor));
}

LinkResult LinkEvaluator::evaluate(const TerrainProfile& profile, Hertz frequency, const TransmitterEnd& tx,
                                   const ReceiverEnd& rx) const {
    const PathGeometry geometry(profile, earth_, tx.antennaAgl, rx.antennaAgl);
    const Metres lambda = wavelength(frequency);
    const DiffractionResult diff = diffraction_.compute(geometry, lambda);

    LinkResult out{};
    out.rayLength = geometry.rayLength();
    out.diffraction = diff;

    const Decibel fs = FreeSpaceLoss::compute(out.rayLength, frequency);
    out.terms.push_back({"tx_gain", "TX antenna gain", -Decibel(tx.antennaGain.value()), "radio profile"});
    out.terms.push_back({"tx_feeder", "TX feeder loss", tx.feederLoss, "radio profile"});
    out.terms.push_back({"free_space", "Free-space loss", fs, "ITU-R P.525"});
    out.terms.push_back({"diffraction", "Terrain diffraction", diff.loss, "ITU-R P.526 Bullington, exact J(ν)"});
    if (diffraction_.options().useEmpiricalCorrection) {
        out.terms.push_back({"diffraction_correction", "Bullington empirical correction",
                             BullingtonDiffraction::empiricalCorrection(diff.loss, profile.length()),
                             "ITU-R P.526 §4.5 (curve fit)"});
    }

    const PathContext context{profile, geometry, diff, frequency};
    for (const auto& c : contributors_) {
        for (BudgetTerm& term : c->contribute(context)) {
            out.terms.push_back(std::move(term));
        }
        for (UncertaintyNote& note : c->notes(context)) {
            out.uncertainty.push_back(std::move(note));
        }
    }

    out.terms.push_back({"rx_gain", "RX antenna gain", -Decibel(rx.antennaGain.value()), "radio profile"});
    out.terms.push_back({"rx_feeder", "RX feeder loss", rx.feederLoss, "radio profile"});

    Decibel total(0.0);
    for (const BudgetTerm& term : out.terms) {
        total += term.value;
    }
    out.received = tx.power - total;
    out.required = rx.sensitivity + rx.requiredMargin;
    out.margin = out.received - out.required;

    Metres worst(0.0);
    for (const ProfileSample& s : profile.samples()) {
        worst = std::max(worst, s.uncertainty);
    }
    out.uncertainty.insert(out.uncertainty.begin(),
                           {"Terrain data", profile.sourceName() + ", stated vertical error up to " +
                                                std::to_string(worst.value()) + " m on this path"});
    return out;
}

}  // namespace rfcore
