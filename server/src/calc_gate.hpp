#pragma once

// The bulkhead for synchronous calculations: POST /calc/path and POST /calc/route run on the
// HTTP request threads and a route may take tens of seconds of one core (calc.maxRouteSamples).
// Without a gate a burst of long routes holds every request thread, and the health check that
// tells a busy server from a dead one waits behind them.

#include "bulkhead.hpp"
#include "config.hpp"

namespace rfmap {

class CalcGate : public Bulkhead {
public:
    /// The limits `calc` asks for; 0 means automatic: one calculation per CPU core, at most half
    /// the request threads, with a queue of the same size but at most a quarter of them, so the
    /// calculations never take more than three quarters of the request threads.
    [[nodiscard]] static Limits limitsFor(const Config::Calc& calc, int serverThreads, unsigned hardwareThreads);

    explicit CalcGate(Limits limits);
};

}  // namespace rfmap
