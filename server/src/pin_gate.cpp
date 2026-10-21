#include "pin_gate.hpp"

#include <algorithm>
#include <stdexcept>

namespace rfmap {

PinGate::Limits PinGate::limitsFor(const Config::Auth& auth, int serverThreads, unsigned hardwareThreads) {
    Limits l;
    const int cores = std::max(1, static_cast<int>(hardwareThreads));
    const int automatic = std::clamp(cores / 2, 1, std::max(1, serverThreads / 4));
    l.concurrency = auth.pinConcurrency > 0 ? auth.pinConcurrency : automatic;
    l.queue = auth.pinQueue > 0 ? auth.pinQueue : l.concurrency;
    l.wait = std::chrono::milliseconds(auth.pinWaitMs);
    return l;
}

PinGate::PinGate(Limits limits) : limits_(limits) {
    if (limits_.concurrency < 1) throw std::invalid_argument("PIN gate needs at least one slot");
    if (limits_.queue < 0 || limits_.wait.count() < 0) throw std::invalid_argument("PIN gate queue and wait must be >= 0");
}

PinGate::Permit::~Permit() {
    if (gate_ != nullptr) gate_->leave();
}

PinGate::Permit PinGate::enter() {
    std::unique_lock lock(mutex_);
    // A newcomer takes a free slot only when nobody queues; otherwise it queues behind them.
    if (active_ < limits_.concurrency && waiting_ == 0) {
        ++active_;
        return Permit(*this);
    }
    if (waiting_ >= limits_.queue) reject();
    ++waiting_;
    const bool admitted = freed_.wait_for(
        lock, limits_.wait, [this] { return handedOver_ > 0 || active_ < limits_.concurrency; });
    --waiting_;
    if (!admitted) reject();
    if (handedOver_ > 0) {
        --handedOver_;  // the slot was kept counted for us
    } else {
        ++active_;
    }
    return Permit(*this);
}

void PinGate::leave() {
    {
        std::lock_guard lock(mutex_);
        // Hand the slot to a queued caller instead of freeing it, so a newcomer cannot take it first.
        if (waiting_ > handedOver_) {
            ++handedOver_;
        } else {
            --active_;
        }
    }
    freed_.notify_one();
}

void PinGate::reject() {
    ++rejected_;
    // Long enough for the slots to turn over: the wait, or a second when callers do not wait.
    const auto retryAfterS = std::max<long long>(1, (limits_.wait.count() + 999) / 1000);
    throw HttpError(503, "the server is busy checking PINs; try again in a few seconds", {{"retryAfterS", retryAfterS}});
}

PinGate::Stats PinGate::stats() const {
    std::lock_guard lock(mutex_);
    return {active_, waiting_, rejected_};
}

}  // namespace rfmap
