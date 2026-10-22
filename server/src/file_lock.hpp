#pragma once

// Locks that other processes see: the running server and an `rfmap-server --backup` started next
// to it share the module files, and an in-process mutex does not reach the second process.

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string_view>

namespace rfmap {

/// An flock(2) lock on a file, held from construction to destruction. The file is created when
/// missing; a lock on it waits for the other processes' locks it conflicts with.
class FileLock {
public:
    enum class Mode { Shared, Exclusive };

    FileLock(const std::filesystem::path& file, Mode mode);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int fd_;
};

/// The gate of the installed modules. Requests hold it shared and admin changes exclusively, as
/// with a shared_mutex; an exclusive hold also takes the lock file in the module root, so a
/// backup in another process (which takes it shared) never reads the files half-changed.
class ModuleGate {
public:
    static constexpr std::string_view kLockFile = ".rfmap-lock";

    /// Takes the lock file in `moduleRoot` from now on.
    void attach(const std::filesystem::path& moduleRoot) { lockFile_ = moduleRoot / kLockFile; }

    void lock();
    void unlock();
    void lock_shared() { mutex_.lock_shared(); }
    void unlock_shared() { mutex_.unlock_shared(); }

private:
    std::shared_mutex mutex_;
    std::filesystem::path lockFile_;
    std::optional<FileLock> held_;
};

}  // namespace rfmap
