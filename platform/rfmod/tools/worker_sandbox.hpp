#pragma once

#include <string>

/// The confinement rfmod-worker puts itself under before it runs module code. Each measure is
/// attempted on its own and reported as it is; the host decides whether the result is enough.
class WorkerSandbox {
public:
    struct Report {
        bool noNewPrivileges = false;
        bool networkIsolated = false;    ///< no sockets: a namespace without routes, or the syscall filter
        bool filesystemIsolated = false; ///< Landlock: the module directory and the system libraries are readable, nothing else is reachable
        bool syscallFilter = false;      ///< seccomp: no sockets, processes, signals to others, ptrace, mounts, ...
        std::string notes;               ///< why a measure failed, for the host log

        /// Every measure a third-party module needs (ModulePolicy::requireSandbox on the host).
        [[nodiscard]] bool complete() const { return noNewPrivileges && networkIsolated && filesystemIsolated && syscallFilter; }
    };

    /// `moduleDir` stays readable, with the system library directories the dynamic loader
    /// searches; the sandbox is applied before the module's library is loaded.
    explicit WorkerSandbox(std::string moduleDir);

    /// Applies every measure in the order they depend on each other and returns what held.
    [[nodiscard]] Report apply();

private:
    bool unshareNetwork();
    bool noNewPrivileges();
    bool landlock();
    /// Grants read access beneath `path` (a directory or a file); a missing path is not an error.
    static bool allowRead(int ruleset, const char* path, bool mustExist);
    bool seccomp();
    void note(const char* measure, const char* detail);

    std::string moduleDir_;
    Report report_;
};
