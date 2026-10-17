#pragma once

#include "rfcore/units.hpp"

namespace rfcore {

/// Receiver sensitivity from first principles:
/// S = 10 log10(k T0 B) + 30 + NF + SNR_required   [dBm]
/// B, NF and the required SNR come from the radio profile in the reference library.
class ReceiverSensitivity {
public:
    [[nodiscard]] static DecibelMilliwatt thermalNoiseFloor(Hertz bandwidth);
    [[nodiscard]] static DecibelMilliwatt compute(Hertz bandwidth, Decibel noiseFigure, Decibel requiredSnr);
};

}  // namespace rfcore
