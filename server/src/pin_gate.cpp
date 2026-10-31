#include "pin_gate.hpp"

#include <algorithm>

namespace rfmap {

PinGate::Limits PinGate::limitsFor(const Config::Auth& auth, int serverThreads, unsigned hardwareThreads) {
    Limits l;
    const int cores = std::max(1, static_cast<int>(hardwareThreads));
    const int automatic = std::clamp(cores / 2, 1, std::max(1, serverThreads / 4));
    l.concurrency = auth.pinConcurrency > 0 ? auth.pinConcurrency : automatic;
    // The automatic queue never takes the last request thread, so a small server still answers others.
    const int spare = std::max(0, serverThreads - 1 - l.concurrency);
    l.queue = auth.pinQueue > 0 ? auth.pinQueue : std::min(l.concurrency, spare);
    l.wait = std::chrono::milliseconds(auth.pinWaitMs);
    return l;
}

PinGate::PinGate(Limits limits)
    : Bulkhead(limits, "the server is busy checking PINs; try again in a few seconds", "PIN gate") {}

}  // namespace rfmap
