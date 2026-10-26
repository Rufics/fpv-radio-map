#include "calc_gate.hpp"

#include <algorithm>

namespace rfmap {

CalcGate::Limits CalcGate::limitsFor(const Config::Calc& calc, int serverThreads, unsigned hardwareThreads) {
    Limits l;
    const int cores = std::max(1, static_cast<int>(hardwareThreads));
    const int automatic = std::clamp(cores, 1, std::max(1, serverThreads / 2));
    l.concurrency = calc.concurrency > 0 ? calc.concurrency : automatic;
    // The automatic queue never takes the last request thread, so a small server still answers others.
    const int spare = std::max(0, serverThreads - 1 - l.concurrency);
    l.queue = calc.queue > 0 ? calc.queue : std::min({l.concurrency, serverThreads / 4, spare});
    l.wait = std::chrono::milliseconds(calc.waitMs);
    return l;
}

CalcGate::CalcGate(Limits limits)
    : Bulkhead(limits, "the server is busy with other calculations; try again in a few seconds", "calculation gate") {}

}  // namespace rfmap
