#include "lifecycle.hpp"

#include <algorithm>

using namespace std::chrono_literals;

namespace rfmap {

GracefulStop::GracefulStop(std::chrono::milliseconds drain, Phase onDrain, Phase onStop)
    : drain_(drain), onDrain_(std::move(onDrain)), onStop_(std::move(onStop)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

GracefulStop::~GracefulStop() {
    thread_.request_stop();
    wake_.notify_all();
}

bool GracefulStop::sleepUnlessStopped(const std::stop_token& stop, std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    // Wakes on the thread's stop request; the predicate is false until then, so this is a sleep.
    return !wake_.wait_for(lock, stop, duration, [] { return false; }) && !stop.stop_requested();
}

bool GracefulStop::drain(const std::stop_token& stop) {
    const auto until = std::chrono::steady_clock::now() + drain_;
    while (!hurried_.load(std::memory_order_relaxed)) {
        const auto left = until - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) break;
        const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(left) + 1ms, kPoll);
        if (!sleepUnlessStopped(stop, slice)) return false;
    }
    return true;
}

void GracefulStop::run(const std::stop_token& stop) {
    while (!requested()) {
        if (!sleepUnlessStopped(stop, kPoll)) return;
    }
    draining_.store(true);
    if (onDrain_) onDrain_();
    if (!drain(stop)) return;
    if (onStop_) onStop_();
    stopped_.store(true);
}

}  // namespace rfmap
