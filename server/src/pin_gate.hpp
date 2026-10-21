#pragma once

// A bulkhead for PIN hashing. argon2id is deliberately slow and memory-hard (the moderate cost
// takes 256 MiB and most of a second of one core), and it runs on the HTTP request threads. A
// burst of link opens could otherwise put every request thread inside argon2id, so health checks
// and map viewers wait behind it. The gate lets a few hashes run at once, lets a few more wait a
// short while for a slot, and turns the rest away at once with 503 and Retry-After.

#include "config.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rfmap {

class PinGate {
public:
    struct Limits {
        int concurrency = 1;                   ///< hashes running at once
        int queue = 1;                         ///< callers that may wait for a slot
        std::chrono::milliseconds wait{2000};  ///< how long one of them waits
    };

    /// The limits `auth` asks for; 0 means automatic: half the CPU cores, at most a quarter of the
    /// request threads, so PIN work never takes more than half of them while others are served.
    [[nodiscard]] static Limits limitsFor(const Config::Auth& auth, int serverThreads, unsigned hardwareThreads);

    explicit PinGate(Limits limits);

    /// One admitted hash. Leaving its scope frees the slot for the next caller.
    class Permit {
    public:
        ~Permit();
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept : gate_(other.gate_) { other.gate_ = nullptr; }
        Permit& operator=(Permit&&) = delete;

    private:
        friend class PinGate;
        explicit Permit(PinGate& gate) : gate_(&gate) {}
        PinGate* gate_;
    };

    /// A slot for one hash. Throws HttpError 503 {retryAfterS} when every slot is busy and the
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
    [[nodiscard]] const Limits& limits() const { return limits_; }

private:
    void leave();
    [[noreturn]] void reject();

    Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable freed_;
    int active_ = 0;
    int waiting_ = 0;
    int handedOver_ = 0;  ///< slots a leaving caller passed to queued callers, not yet taken
    std::uint64_t rejected_ = 0;
};

}  // namespace rfmap
