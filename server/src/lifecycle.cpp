#include "lifecycle.hpp"

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

void GracefulStop::run(const std::stop_token& stop) {
    while (!requested()) {
        if (!sleepUnlessStopped(stop, kPoll)) return;
    }
    draining_.store(true);
    if (onDrain_) onDrain_();
    if (!sleepUnlessStopped(stop, drain_)) return;
    if (onStop_) onStop_();
    stopped_.store(true);
}

}  // namespace rfmap
