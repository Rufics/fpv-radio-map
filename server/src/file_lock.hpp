#pragma once

// Locks that other processes see: the running server and an `rfmap-server --backup` started next
// to it share the module files, and an in-process mutex does not reach the second process.

#include <filesystem>
#include <memory>
#include <mutex>
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

/// The gate of the installed modules. Requests hold a Read and admin changes a Change, as with
/// a shared_mutex; a Change also takes the lock file in the module root, so a backup in another
/// process (which takes it shared) never reads the files half-changed. The lock file comes first,
/// so requests keep running while an admin change waits for a backup.
class ModuleGate {
public:
    static constexpr std::string_view kLockFile = ".rfmap-lock";

    /// Takes the lock file in `moduleRoot` from now on.
    void attach(const std::filesystem::path& moduleRoot) { lockFile_ = moduleRoot / kLockFile; }

    /// A request: the installed modules stay as they are while it is held.
    class Read {
    public:
        explicit Read(ModuleGate& gate) : lock_(gate.mutex_) {}

    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    /// An admin change: no request and no backup sees the modules while it is held.
    class Change {
    public:
        explicit Change(ModuleGate& gate) : file_(gate.lockFile()), lock_(gate.mutex_) {}

    private:
        std::unique_ptr<FileLock> file_;  // before lock_: taken first, released last
        std::unique_lock<std::shared_mutex> lock_;
    };

private:
    [[nodiscard]] std::unique_ptr<FileLock> lockFile() const;

    std::shared_mutex mutex_;
    std::filesystem::path lockFile_;
};

}  // namespace rfmap
