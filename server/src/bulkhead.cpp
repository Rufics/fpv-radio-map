#include "bulkhead.hpp"

#include "common.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rfmap {

Bulkhead::Bulkhead(Limits limits, std::string busyMessage, const std::string& name)
    : limits_(limits), busyMessage_(std::move(busyMessage)) {
    if (limits_.concurrency < 1) throw std::invalid_argument(name + " needs at least one slot");
    if (limits_.queue < 0 || limits_.wait.count() < 0) throw std::invalid_argument(name + " queue and wait must be >= 0");
}

Bulkhead::Permit::~Permit() {
    if (gate_ != nullptr) gate_->leave();
}

Bulkhead::Permit Bulkhead::enter() {
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

void Bulkhead::leave() {
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

void Bulkhead::reject() {
    ++rejected_;
    // Long enough for the slots to turn over: the wait, or a second when callers do not wait.
    const auto retryAfterS = std::max<long long>(1, (limits_.wait.count() + 999) / 1000);
    throw HttpError(503, busyMessage_, {{"retryAfterS", retryAfterS}});
}

Bulkhead::Stats Bulkhead::stats() const {
    std::lock_guard lock(mutex_);
    return {active_, waiting_, rejected_};
}

}  // namespace rfmap
