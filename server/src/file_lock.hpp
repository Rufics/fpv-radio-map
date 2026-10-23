#pragma once

// The server's side of the module root lock (rfmod/file_lock.hpp): the running server and an
// `rfmap-server --backup` started next to it share the module files.

#include <rfmod/file_lock.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>

namespace rfmap {

using rfmod::FileLock;

/// The gate of the module files. Every change the server makes to them (a reload, which may
/// restore a file, or new settings) holds a Change, which also takes the lock file in the module
/// root, so a backup in another process (which takes it shared) never reads them half-changed.
/// Requests need no gate: they keep the module instances they started with.
class ModuleGate {
public:
    static constexpr std::string_view kLockFile = rfmod::kRootLockFile;

    /// Takes the lock file in `moduleRoot` from now on.
    void attach(const std::filesystem::path& moduleRoot) { lockFile_ = moduleRoot / kLockFile; }

    /// A change to the module files: no backup reads them while it is held.
    class Change {
    public:
        explicit Change(ModuleGate& gate) : file_(gate.lockFile()), lock_(gate.mutex_) {}

    private:
        std::unique_ptr<FileLock> file_;  // before lock_: taken first, released last
        std::unique_lock<std::mutex> lock_;
    };

private:
    [[nodiscard]] std::unique_ptr<FileLock> lockFile() const;

    std::mutex mutex_;
    std::filesystem::path lockFile_;
};

}  // namespace rfmap
