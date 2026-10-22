#pragma once

#include <sys/resource.h>
#include <sys/types.h>

#include <cstddef>

namespace rfmod {

/// The child side of starting rfmod-worker. It runs between fork and exec, so it makes only
/// async-signal-safe calls, and it never returns: it becomes the worker or exits with 127.
class WorkerExec {
public:
    /// Everything the child needs, prepared before fork.
    struct Plan {
        char* const* argv;  ///< worker path, library path, memory limit; null-terminated
        char* const* envp;  ///< null-terminated
        const char* dir;    ///< the module directory: the worker runs in it and may read nothing else
        int socketFd;       ///< the worker's end of the protocol socket
        int errorFd;        ///< write end of the pipe that carries the worker's stdout and stderr
        rlim_t memBytes;    ///< address-space limit
        pid_t hostPid;      ///< the host's pid, taken before fork: the worker must still be its child
    };

    static constexpr int kProtocolFd = 3;  ///< the worker's end of the socket, see tools/rfmod-worker.cpp

    [[noreturn]] static void run(const Plan& plan) noexcept;

private:
    static bool redirectDescriptors(const Plan& plan) noexcept;
    static bool applyLimits(rlim_t memBytes) noexcept;
    /// The worker leads its own process group and dies with the host, which must still be alive.
    static bool detachFromHost(pid_t hostPid) noexcept;

    // Takes only string literals: their length is known at compile time and nothing has to scan memory.
    template <std::size_t N>
    [[noreturn]] static void fail(int fd, const char (&what)[N]) noexcept;
};

}  // namespace rfmod
