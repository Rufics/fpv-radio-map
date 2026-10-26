#include "lifecycle.hpp"

#include <algorithm>

using namespace std::chrono_literals;

namespace rfmap {

GracefulStop::GracefulStop(std::chrono::milliseconds drain, Phase onDrain, Phase onStop)
    : drain_(drain), onDrain_(std::move(onDrain)), onStop_(std::move(onStop)), thread_([this] { run(); }) {}

GracefulStop::~GracefulStop() {
    {
        const std::lock_guard lock(mutex_);
        ending_ = true;
    }
    wake_.notify_all();
}

bool GracefulStop::sleepUnlessStopped(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    // Wakes early only when the destructor sets ending_; otherwise this is a sleep.
    return !wake_.wait_for(lock, duration, [this] { return ending_; });
}

bool GracefulStop::drain() {
    const auto until = std::chrono::steady_clock::now() + drain_;
    while (!hurried_.load()) {
        const auto left = until - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) break;
        const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(left) + 1ms, kPoll);
        if (!sleepUnlessStopped(slice)) return false;
    }
    return true;
}

void GracefulStop::run() {
    while (!requested()) {
        if (!sleepUnlessStopped(kPoll)) return;
    }
    draining_.store(true);
    if (onDrain_) onDrain_();
    if (!drain()) return;
    if (onStop_) onStop_();
    stopped_.store(true);
}

}  // namespace rfmap
