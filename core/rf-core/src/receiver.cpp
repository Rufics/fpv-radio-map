#include "rfcore/receiver.hpp"

#include "rfcore/constants.hpp"

#include <cmath>
#include <stdexcept>

namespace rfcore {

DecibelMilliwatt ReceiverSensitivity::thermalNoiseFloor(Hertz bandwidth) {
    if (!(bandwidth.value() > 0.0)) {
        throw std::invalid_argument("ReceiverSensitivity: bandwidth must be positive");
    }
    const double watts =
        constants::kBoltzmann * constants::kReferenceNoiseTemperature.value() * bandwidth.value();
    return DecibelMilliwatt(10.0 * std::log10(watts) + 30.0);
}

DecibelMilliwatt ReceiverSensitivity::compute(Hertz bandwidth, Decibel noiseFigure, Decibel requiredSnr) {
    return thermalNoiseFloor(bandwidth) + noiseFigure + requiredSnr;
}

}  // namespace rfcore
