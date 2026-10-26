#pragma once

// How the server stops. A load balancer learns that an instance is leaving from its readiness
// probe, and only after a few seconds does it stop sending requests there. So the server does
// not stop the moment it is told to: it first reports itself not ready and keeps serving for a
// drain window, then stops listening and lets the requests in flight finish.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace rfmap {

/// Runs the two phases of a stop on a thread of its own once `request` is called. `request` is
/// async-signal-safe: it only sets atomic flags that the thread polls, so a signal handler may
/// call it. Destroying the object ends the thread without running phases it has not reached.
class GracefulStop {
public:
    using Phase = std::function<void()>;

    /// `onDrain` runs as soon as a stop is requested (mark not ready); `onStop` runs `drain` later.
    GracefulStop(std::chrono::milliseconds drain, Phase onDrain, Phase onStop);
    ~GracefulStop();
    GracefulStop(const GracefulStop&) = delete;
    GracefulStop& operator=(const GracefulStop&) = delete;
    GracefulStop(GracefulStop&&) = delete;
    GracefulStop& operator=(GracefulStop&&) = delete;

    /// Asks for a stop. Safe from a signal handler: it only sets atomic flags. A second call
    /// (a second Ctrl-C) cuts the drain short, so the stop phase runs at the next poll.
    void request() noexcept {
        if (requested_.exchange(true)) hurried_.store(true);
    }
    [[nodiscard]] bool requested() const noexcept { return requested_.load(); }
    /// True from the moment the drain phase began.
    [[nodiscard]] bool draining() const noexcept { return draining_.load(); }
    /// True once the stop phase has run.
    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(); }

private:
    static constexpr std::chrono::milliseconds kPoll{50};
    void run();
    /// Sleeps `duration` or less; false once the object is being destroyed.
    [[nodiscard]] bool sleepUnlessStopped(std::chrono::milliseconds duration);
    /// Waits out the drain window in polls, ending early on a second request. False if the
    /// object is being destroyed.
    [[nodiscard]] bool drain();

    std::chrono::milliseconds drain_;
    Phase onDrain_;
    Phase onStop_;
    std::atomic<bool> requested_{false};
    std::atomic<bool> hurried_{false};
    std::atomic<bool> draining_{false};
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool ending_ = false;  ///< set by the destructor, under mutex_
    std::jthread thread_;  ///< declared last: started after every member it reads exists, joined first
};

}  // namespace rfmap
