#pragma once

// A bulkhead for slow work that runs on the HTTP request threads. A few callers run at once, a
// few more wait a short while for a slot, and the rest are turned away at once with 503 and
// Retry-After, so a burst of one kind of request cannot hold every request thread while health
// checks and map viewers wait behind it. PinGate and CalcGate are the two configurations.

#include "metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace rfmap {

class Bulkhead {
public:
    struct Limits {
        int concurrency = 1;                   ///< callers running at once
        int queue = 1;                         ///< callers that may wait for a slot
        std::chrono::milliseconds wait{2000};  ///< how long one of them waits
    };

    /// `busyMessage` is what a turned-away caller reads; `name` names the gate in a limits error.
    Bulkhead(Limits limits, std::string busyMessage, const std::string& name);

    /// One admitted caller. Leaving its scope frees the slot for the next caller.
    class Permit {
    public:
        ~Permit();
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept : gate_(other.gate_) { other.gate_ = nullptr; }
        Permit& operator=(Permit&&) = delete;

    private:
        friend class Bulkhead;
        explicit Permit(Bulkhead& gate) : gate_(&gate) {}
        Bulkhead* gate_;
    };

    /// A slot for one caller. Throws HttpError 503 {retryAfterS} when every slot is busy and the
    /// queue is full, or when no slot frees up within the wait.
    [[nodiscard]] Permit enter();

    /// Runs `work` inside a slot and returns what it returns.
    template <class Work>
    auto run(const Work& work) {
        const Permit permit = enter();
        return work();
    }

    struct Stats {
        int active{};
        int waiting{};
        std::uint64_t rejected{};
    };
    [[nodiscard]] Stats stats() const;
    /// The gate as the metrics see it, under `name`.
    [[nodiscard]] GateSample sample(std::string name) const;
    [[nodiscard]] const Limits& limits() const { return limits_; }

private:
    void leave();
    [[noreturn]] void reject();

    Limits limits_;
    std::string busyMessage_;
    mutable std::mutex mutex_;
    std::condition_variable freed_;
    int active_ = 0;
    int waiting_ = 0;
    int handedOver_ = 0;  ///< slots a leaving caller passed to queued callers, not yet taken
    std::uint64_t rejected_ = 0;
};

}  // namespace rfmap
