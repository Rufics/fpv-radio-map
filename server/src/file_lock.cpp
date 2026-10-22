#include "file_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace rfmap {

FileLock::FileLock(const std::filesystem::path& file, Mode mode)
    : fd_(::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "cannot open " + file.string());
    const int operation = mode == Mode::Exclusive ? LOCK_EX : LOCK_SH;
    int result = ::flock(fd_, operation);
    while (result != 0 && errno == EINTR) result = ::flock(fd_, operation);
    if (result != 0) {
        const int error = errno;
        ::close(fd_);
        throw std::system_error(error, std::generic_category(), "cannot lock " + file.string());
    }
}

FileLock::~FileLock() {
    ::close(fd_);  // closing the descriptor releases the lock
}

std::unique_ptr<FileLock> ModuleGate::lockFile() const {
    if (lockFile_.empty()) return nullptr;
    std::filesystem::create_directories(lockFile_.parent_path());
    return std::make_unique<FileLock>(lockFile_, FileLock::Mode::Exclusive);
}

}  // namespace rfmap
