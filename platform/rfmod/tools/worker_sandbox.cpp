#include "worker_sandbox.hpp"

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

// Landlock constants newer than the kernel headers this is built against. The values are ABI.
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#endif
#ifndef LANDLOCK_ACCESS_NET_CONNECT_TCP
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#endif
#ifndef LANDLOCK_SCOPE_SIGNAL
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif

namespace {

/// The struct landlock_ruleset_attr of ABI 6, so the build does not depend on the header's age.
/// The kernel reads only the first `size` bytes it is given.
struct RulesetAttr {
    __u64 handledAccessFs;
    __u64 handledAccessNet;
    __u64 scoped;
};

struct PathBeneathAttr {
    __u64 allowedAccess;
    __s32 parentFd;
} __attribute__((packed));

/// The seccomp program: a denylist. Anything not named is allowed, so the module keeps memory,
/// time, futexes and the protocol socket; what it loses is every way to reach outside the process.
class SyscallFilter {
public:
    explicit SyscallFilter(pid_t self) : self_(static_cast<__u32>(self)) {}

    [[nodiscard]] std::vector<sock_filter> build() {
        checkArchitecture();
        loadSyscallNumber();
        denyAll(kSocketSyscalls);
        denyAll(kProcessSyscalls);
        denyAll(kPrivilegeSyscalls);
        denyAll(kKernelSyscalls);
        denyAll(kFileMetadataSyscalls);
        denySignalsToOthers();
        denyNewProcesses();
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        return program_;
    }

private:
    static constexpr __u32 kDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

    static constexpr int kSocketSyscalls[] = {
#ifdef __NR_socket
        __NR_socket,
#endif
#ifdef __NR_socketpair
        __NR_socketpair,
#endif
#ifdef __NR_connect
        __NR_connect,
#endif
#ifdef __NR_bind
        __NR_bind,
#endif
#ifdef __NR_listen
        __NR_listen,
#endif
#ifdef __NR_accept
        __NR_accept,
#endif
#ifdef __NR_accept4
        __NR_accept4,
#endif
#ifdef __NR_sendto
        __NR_sendto,
#endif
#ifdef __NR_sendmsg
        __NR_sendmsg,
#endif
#ifdef __NR_sendmmsg
        __NR_sendmmsg,
#endif
#ifdef __NR_recvfrom
        __NR_recvfrom,
#endif
#ifdef __NR_recvmsg
        __NR_recvmsg,
#endif
#ifdef __NR_recvmmsg
        __NR_recvmmsg,
#endif
#ifdef __NR_setsockopt
        __NR_setsockopt,
#endif
#ifdef __NR_getsockopt
        __NR_getsockopt,
#endif
#ifdef __NR_shutdown
        __NR_shutdown,
#endif
#ifdef __NR_io_uring_setup
        __NR_io_uring_setup,
#endif
    };

    static constexpr int kProcessSyscalls[] = {
#ifdef __NR_fork
        __NR_fork,
#endif
#ifdef __NR_vfork
        __NR_vfork,
#endif
#ifdef __NR_execve
        __NR_execve,
#endif
#ifdef __NR_execveat
        __NR_execveat,
#endif
#ifdef __NR_ptrace
        __NR_ptrace,
#endif
#ifdef __NR_process_vm_readv
        __NR_process_vm_readv,
#endif
#ifdef __NR_process_vm_writev
        __NR_process_vm_writev,
#endif
#ifdef __NR_pidfd_open
        __NR_pidfd_open,
#endif
#ifdef __NR_pidfd_getfd
        __NR_pidfd_getfd,
#endif
#ifdef __NR_pidfd_send_signal
        __NR_pidfd_send_signal,
#endif
#ifdef __NR_rt_sigqueueinfo
        __NR_rt_sigqueueinfo,
#endif
#ifdef __NR_rt_tgsigqueueinfo
        __NR_rt_tgsigqueueinfo,
#endif
#ifdef __NR_unshare
        __NR_unshare,
#endif
#ifdef __NR_setns
        __NR_setns,
#endif
    };

    static constexpr int kPrivilegeSyscalls[] = {
#ifdef __NR_setuid
        __NR_setuid,
#endif
#ifdef __NR_setgid
        __NR_setgid,
#endif
#ifdef __NR_setreuid
        __NR_setreuid,
#endif
#ifdef __NR_setregid
        __NR_setregid,
#endif
#ifdef __NR_setresuid
        __NR_setresuid,
#endif
#ifdef __NR_setresgid
        __NR_setresgid,
#endif
#ifdef __NR_setgroups
        __NR_setgroups,
#endif
#ifdef __NR_setfsuid
        __NR_setfsuid,
#endif
#ifdef __NR_setfsgid
        __NR_setfsgid,
#endif
#ifdef __NR_capset
        __NR_capset,
#endif
#ifdef __NR_keyctl
        __NR_keyctl,
#endif
#ifdef __NR_add_key
        __NR_add_key,
#endif
#ifdef __NR_request_key
        __NR_request_key,
#endif
    };

    static constexpr int kKernelSyscalls[] = {
#ifdef __NR_mount
        __NR_mount,
#endif
#ifdef __NR_umount2
        __NR_umount2,
#endif
#ifdef __NR_pivot_root
        __NR_pivot_root,
#endif
#ifdef __NR_chroot
        __NR_chroot,
#endif
#ifdef __NR_open_tree
        __NR_open_tree,
#endif
#ifdef __NR_move_mount
        __NR_move_mount,
#endif
#ifdef __NR_fsopen
        __NR_fsopen,
#endif
#ifdef __NR_fsmount
        __NR_fsmount,
#endif
#ifdef __NR_mount_setattr
        __NR_mount_setattr,
#endif
#ifdef __NR_reboot
        __NR_reboot,
#endif
#ifdef __NR_kexec_load
        __NR_kexec_load,
#endif
#ifdef __NR_kexec_file_load
        __NR_kexec_file_load,
#endif
#ifdef __NR_init_module
        __NR_init_module,
#endif
#ifdef __NR_finit_module
        __NR_finit_module,
#endif
#ifdef __NR_delete_module
        __NR_delete_module,
#endif
#ifdef __NR_bpf
        __NR_bpf,
#endif
#ifdef __NR_perf_event_open
        __NR_perf_event_open,
#endif
#ifdef __NR_userfaultfd
        __NR_userfaultfd,
#endif
#ifdef __NR_sethostname
        __NR_sethostname,
#endif
#ifdef __NR_setdomainname
        __NR_setdomainname,
#endif
#ifdef __NR_settimeofday
        __NR_settimeofday,
#endif
#ifdef __NR_clock_settime
        __NR_clock_settime,
#endif
#ifdef __NR_adjtimex
        __NR_adjtimex,
#endif
#ifdef __NR_clock_adjtime
        __NR_clock_adjtime,
#endif
#ifdef __NR_acct
        __NR_acct,
#endif
#ifdef __NR_swapon
        __NR_swapon,
#endif
#ifdef __NR_swapoff
        __NR_swapoff,
#endif
#ifdef __NR_quotactl
        __NR_quotactl,
#endif
#ifdef __NR_syslog
        __NR_syslog,
#endif
#ifdef __NR_setpriority
        __NR_setpriority,
#endif
#ifdef __NR_sched_setaffinity
        __NR_sched_setaffinity,
#endif
#ifdef __NR_ioperm
        __NR_ioperm,
#endif
#ifdef __NR_iopl
        __NR_iopl,
#endif
#ifdef __NR_modify_ldt
        __NR_modify_ldt,
#endif
#ifdef __NR_seccomp
        __NR_seccomp,  // no second filter on top of this one, which a module could use to confuse the host
#endif
    };

    /// What Landlock does not cover: changing a file's mode, owner, times and attributes, and
    /// truncation before ABI 3. The module reads files; it never changes them.
    static constexpr int kFileMetadataSyscalls[] = {
#ifdef __NR_truncate
        __NR_truncate,
#endif
#ifdef __NR_ftruncate
        __NR_ftruncate,
#endif
#ifdef __NR_fallocate
        __NR_fallocate,
#endif
#ifdef __NR_chmod
        __NR_chmod,
#endif
#ifdef __NR_fchmod
        __NR_fchmod,
#endif
#ifdef __NR_fchmodat
        __NR_fchmodat,
#endif
#ifdef __NR_fchmodat2
        __NR_fchmodat2,
#endif
#ifdef __NR_chown
        __NR_chown,
#endif
#ifdef __NR_fchown
        __NR_fchown,
#endif
#ifdef __NR_lchown
        __NR_lchown,
#endif
#ifdef __NR_fchownat
        __NR_fchownat,
#endif
#ifdef __NR_utime
        __NR_utime,
#endif
#ifdef __NR_utimes
        __NR_utimes,
#endif
#ifdef __NR_futimesat
        __NR_futimesat,
#endif
#ifdef __NR_utimensat
        __NR_utimensat,
#endif
#ifdef __NR_setxattr
        __NR_setxattr,
#endif
#ifdef __NR_lsetxattr
        __NR_lsetxattr,
#endif
#ifdef __NR_fsetxattr
        __NR_fsetxattr,
#endif
#ifdef __NR_removexattr
        __NR_removexattr,
#endif
#ifdef __NR_lremovexattr
        __NR_lremovexattr,
#endif
#ifdef __NR_fremovexattr
        __NR_fremovexattr,
#endif
    };

#if defined(__x86_64__)
    static constexpr __u32 kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    static constexpr __u32 kArch = AUDIT_ARCH_AARCH64;
#else
#error "rfmod-worker: no seccomp architecture constant for this target"
#endif

    void checkArchitecture() {
        program_.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
        program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    }

    void loadSyscallNumber() {
        program_.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
        // The x32 ABI shares the table with different numbers: nothing legitimate uses it here.
        program_.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif
    }

    template <std::size_t N>
    void denyAll(const int (&numbers)[N]) {
        for (const int nr : numbers) {
            program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(nr), 0, 1));
            program_.push_back(BPF_STMT(BPF_RET | BPF_K, kDeny));
        }
    }

    /// Loads the low 32 bits of argument `index` into the accumulator.
    void loadArgument(unsigned index) {
        const auto offset = static_cast<__u32>(offsetof(seccomp_data, args) + index * sizeof(__u64));
        program_.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offset));
    }

    /// kill, tkill and tgkill reach only this process (abort() and raise() keep working).
    void denySignalsToOthers() {
        const int numbers[] = {
#ifdef __NR_kill
            __NR_kill,
#endif
#ifdef __NR_tkill
            __NR_tkill,
#endif
#ifdef __NR_tgkill
            __NR_tgkill,
#endif
        };
        for (const int nr : numbers) {
            // if nr != this syscall: skip 4 instructions
            program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(nr), 0, 4));
            loadArgument(0);  // pid (tkill: tid of a thread in this process, which is != pid; it is denied)
            program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, self_, 0, 1));
            program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
            program_.push_back(BPF_STMT(BPF_RET | BPF_K, kDeny));
            loadSyscallNumberAgain();
        }
    }

    /// Threads stay possible (clone with CLONE_THREAD); new processes do not. clone3 answers
    /// ENOSYS so that glibc falls back to clone, where the flags can be inspected.
    void denyNewProcesses() {
#ifdef __NR_clone3
        program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(__NR_clone3), 0, 1));
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)));
#endif
#ifdef __NR_clone
        program_.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(__NR_clone), 0, 4));
        loadArgument(0);  // flags
        program_.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, static_cast<__u32>(CLONE_THREAD), 0, 1));
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        program_.push_back(BPF_STMT(BPF_RET | BPF_K, kDeny));
        loadSyscallNumberAgain();
#endif
    }

    void loadSyscallNumberAgain() {
        program_.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    }

    __u32 self_;
    std::vector<sock_filter> program_;
};

/// Where glibc's loader looks for a library's dependencies when no path is given: the cache and
/// the system library directories (multiarch directories lie beneath /usr/lib). Read-only and
/// world-readable anyway; whichever of them exist are allowed.
constexpr const char* kLoaderPaths[] = {"/etc/ld.so.cache", "/lib", "/lib64", "/usr/lib", "/usr/lib64", "/usr/local/lib"};

}  // namespace

WorkerSandbox::WorkerSandbox(std::string moduleDir) : moduleDir_(std::move(moduleDir)) {}

void WorkerSandbox::note(const char* measure, const char* detail) {
    if (!report_.notes.empty()) report_.notes += "; ";
    report_.notes += measure;
    report_.notes += ": ";
    report_.notes += detail;
}

WorkerSandbox::Report WorkerSandbox::apply() {
    // Order matters: a namespace needs no earlier step; Landlock and seccomp need no-new-privileges;
    // seccomp goes last because it forbids the syscalls the other steps use.
    const bool namespaced = unshareNetwork();
    report_.noNewPrivileges = noNewPrivileges();
    report_.filesystemIsolated = report_.noNewPrivileges && landlock();
    report_.syscallFilter = report_.noNewPrivileges && seccomp();
    report_.networkIsolated = namespaced || report_.syscallFilter;
    return report_;
}

bool WorkerSandbox::unshareNetwork() {
    // A new user+network namespace leaves the module with only a loopback device and no route out.
    // Where the kernel or container forbids unprivileged namespaces this fails and the syscall
    // filter has to carry network isolation alone.
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0) return true;
    note("namespace", std::strerror(errno));
    return false;
}

bool WorkerSandbox::noNewPrivileges() {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) return true;
    note("no-new-privileges", std::strerror(errno));
    return false;
}

bool WorkerSandbox::landlock() {
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        note("landlock", abi < 0 ? std::strerror(errno) : "unsupported");
        return false;
    }
    // Handle every file system right this kernel knows; unhandled rights would stay allowed.
    __u64 fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
               LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
               LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
               LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
               LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5) fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    RulesetAttr attr{fs, 0, 0};
    std::size_t size = sizeof(attr.handledAccessFs);
    if (abi >= 4) {
        attr.handledAccessNet = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        size = offsetof(RulesetAttr, scoped);
    }
    if (abi >= 6) {
        attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
        size = sizeof(attr);
    }
    const int ruleset = static_cast<int>(syscall(SYS_landlock_create_ruleset, &attr, size, 0));
    if (ruleset < 0) {
        note("landlock", std::strerror(errno));
        return false;
    }
    // The module may read its own directory (its library and the data files next to it) and the
    // system libraries the dynamic loader resolves its dependencies from; nothing else is
    // reachable, and nothing is writable. The loader runs after this, so it needs these.
    bool ok = allowRead(ruleset, moduleDir_.c_str(), true);
    for (const char* path : kLoaderPaths) ok = ok && allowRead(ruleset, path, false);
    ok = ok && syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    if (!ok) note("landlock", std::strerror(errno));
    close(ruleset);
    return ok;
}

bool WorkerSandbox::allowRead(int ruleset, const char* path, bool mustExist) {
    const int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return !mustExist && errno == ENOENT;
    // A directory rule may also grant reading it; on a file that right is refused (EINVAL).
    struct stat st {};
    __u64 access = LANDLOCK_ACCESS_FS_READ_FILE;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) access |= LANDLOCK_ACCESS_FS_READ_DIR;
    const PathBeneathAttr rule{access, fd};
    const bool ok = syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
    close(fd);
    return ok;
}

bool WorkerSandbox::seccomp() {
    std::vector<sock_filter> program = SyscallFilter(getpid()).build();
    const sock_fprog prog{static_cast<unsigned short>(program.size()), program.data()};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) == 0) return true;
    note("seccomp", std::strerror(errno));
    return false;
}
