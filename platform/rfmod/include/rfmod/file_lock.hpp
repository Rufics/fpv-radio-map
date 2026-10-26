#pragma once

// A lock that other processes see. The module root is shared by the running server, the rfmod
// CLI and an `rfmap-server --backup` started next to them; an in-process mutex reaches none of
// the others.

#include <filesystem>
#include <string_view>

namespace rfmod {

/// The file in the module root whose lock guards the files under it: whoever changes them (an
/// install, a remove, a reload that restores a file) holds it exclusively, a reader that needs
/// them unchanging (a backup) holds it shared.
inline constexpr std::string_view kRootLockFile = ".rfmap-lock";

/// The file whose lock guards a trust store, next to it: `rfmod trust add|remove` holds it
/// exclusively from loading the store to saving it, a backup holds it shared while it reads.
[[nodiscard]] inline std::filesystem::path trustStoreLockFile(const std::filesystem::path& store) {
    std::filesystem::path lock = store;
    lock += ".lock";
    return lock;
}

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

}  // namespace rfmod
