#pragma once

// The bulkhead for PIN hashing. argon2id is deliberately slow and memory-hard (the moderate cost
// takes 256 MiB and most of a second of one core), and it runs on the HTTP request threads. A
// burst of link opens could otherwise put every request thread inside argon2id, so health checks
// and map viewers wait behind it.

#include "bulkhead.hpp"
#include "config.hpp"

namespace rfmap {

class PinGate : public Bulkhead {
public:
    /// The limits `auth` asks for; 0 means automatic: half the CPU cores, at most a quarter of the
    /// request threads, so PIN work never takes more than half of them while others are served.
    [[nodiscard]] static Limits limitsFor(const Config::Auth& auth, int serverThreads, unsigned hardwareThreads);

    explicit PinGate(Limits limits);
};

}  // namespace rfmap
