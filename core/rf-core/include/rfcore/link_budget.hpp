#pragma once

#include "rfcore/diffraction.hpp"
#include "rfcore/earth_model.hpp"
#include "rfcore/terrain_profile.hpp"
#include "rfcore/units.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rfcore {

/// One named line of a link budget: what the "why" breakdown shows.
struct BudgetTerm {
    std::string id;      ///< Stable id, for example "free_space" or "rf.clutter/forest".
    std::string label;   ///< Short English label for the UI.
    Decibel value;       ///< Positive = loss, negative = gain.
    std::string source;  ///< Which model or module produced it.
};

/// A known error that is not ours (DEM, forecast, datasheet), carried with the result.
struct UncertaintyNote {
    std::string what;
    std::string detail;
};

struct RadioEnd {
    Metres antennaAgl;
    DecibelIsotropic antennaGain;
    Decibel feederLoss;
};

struct TransmitterEnd : RadioEnd {
    DecibelMilliwatt power;
};

struct ReceiverEnd : RadioEnd {
    DecibelMilliwatt sensitivity;  ///< From ReceiverSensitivity or the radio profile.
    Decibel requiredMargin;        ///< Fade margin the user asks for.
};

/// Everything an extra loss term may look at.
struct PathContext {
    const TerrainProfile& profile;
    const PathGeometry& geometry;
    const DiffractionResult& diffraction;
    Hertz frequency;
};

/// What one contributor adds to a link budget: its terms and the uncertainty it brings.
struct LossContribution {
    std::vector<BudgetTerm> terms;
    std::vector<UncertaintyNote> notes;
};

/// Extension point for RF extensions (weather, clutter, ...). Each contributes zero or more
/// named terms; the core never knows what they model. One call per path, so a contributor
/// keeps no state between the terms and the notes of the same evaluation.
class ILossContributor {
public:
    virtual ~ILossContributor() = default;
    [[nodiscard]] virtual LossContribution contribute(const PathContext& context) const = 0;
};

struct LinkResult {
    Metres rayLength;
    DiffractionResult diffraction;
    std::vector<BudgetTerm> terms;  ///< In display order: gains and losses from TX to RX.
    DecibelMilliwatt received;
    DecibelMilliwatt required;      ///< sensitivity + requiredMargin
    Decibel margin;                 ///< received - required
    std::vector<UncertaintyNote> uncertainty;

    [[nodiscard]] bool reachable() const { return margin.value() >= 0.0; }
};

/// Evaluates one radio link over one terrain profile.
class LinkEvaluator {
public:
    LinkEvaluator(EarthModel earth, BullingtonDiffraction diffraction);

    void addContributor(std::shared_ptr<const ILossContributor> contributor);

    [[nodiscard]] LinkResult evaluate(const TerrainProfile& profile, Hertz frequency, const TransmitterEnd& tx,
                                      const ReceiverEnd& rx) const;

private:
    EarthModel earth_;
    BullingtonDiffraction diffraction_;
    std::vector<std::shared_ptr<const ILossContributor>> contributors_;
};

}  // namespace rfcore
