#include "worker_exec.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace rfmod {

template <std::size_t N>
void WorkerExec::fail(int fd, const char (&what)[N]) noexcept {
    (void)!write(fd, what, N - 1);
    _exit(127);
}

bool WorkerExec::redirectDescriptors(const Plan& plan) noexcept {
    const int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) return false;
    const bool ok = dup2(devnull, 0) >= 0 && dup2(plan.errorFd, 1) >= 0 && dup2(plan.errorFd, 2) >= 0 &&
                    dup2(plan.socketFd, kProtocolFd) >= 0;
    // The original devnull descriptor is not closed here: open() hands out the lowest free number,
    // so it may be one of 0..3 that a dup2 above has just set up; any higher number goes with
    // the close_range in run().
    return ok;
}

bool WorkerExec::applyLimits(rlim_t memBytes) noexcept {
    const rlimit as{memBytes, memBytes};
    const rlimit zero{0, 0};
    const rlimit files{32, 32};
    // No RLIMIT_NPROC: it counts every task of the user, threads included, so it would refuse the
    // module's threads while leaving a root worker free to fork. The seccomp filter in
    // rfmod-worker refuses new processes and keeps threads.
    return setrlimit(RLIMIT_AS, &as) == 0 && setrlimit(RLIMIT_CORE, &zero) == 0 &&
           setrlimit(RLIMIT_FSIZE, &zero) == 0 && setrlimit(RLIMIT_NOFILE, &files) == 0;
}

bool WorkerExec::detachFromHost(pid_t hostPid) noexcept {
    // Its own process group, so the host can kill the whole group; and SIGKILL when the host
    // dies, so a worker never outlives the server that started it. The parent is compared with
    // the host's own pid, not with 1: in a container the host itself may be pid 1.
    return setpgid(0, 0) == 0 && prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0 && getppid() == hostPid;
}

void WorkerExec::run(const Plan& plan) noexcept {
    if (!redirectDescriptors(plan)) fail(plan.errorFd, "rfmod: cannot set up worker descriptors\n");
    // Close everything else the host had open (database sockets, listening sockets, ...).
    close_range(kProtocolFd + 1, ~0u, 0);
    if (!detachFromHost(plan.hostPid)) fail(2, "rfmod: cannot detach the worker from the host\n");
    if (!applyLimits(plan.memBytes)) fail(2, "rfmod: cannot apply resource limits\n");
    if (chdir(plan.dir) != 0) fail(2, "rfmod: cannot enter module directory\n");
    // The worker path is operator configuration, the library path is confined to the registry root
    // and argv is fixed; nothing here is request input.
    execve(plan.argv[0], plan.argv, plan.envp);  // NOSONAR(cpp:S2076, cpp:S2083): see above
    fail(2, "rfmod: cannot start rfmod-worker\n");
}

}  // namespace rfmod
