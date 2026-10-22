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

void ModuleGate::lock() {
    mutex_.lock();
    if (lockFile_.empty()) return;
    try {
        std::filesystem::create_directories(lockFile_.parent_path());
        held_.emplace(lockFile_, FileLock::Mode::Exclusive);
    } catch (const std::system_error&) {
        mutex_.unlock();
        throw;
    }
}

void ModuleGate::unlock() {
    held_.reset();
    mutex_.unlock();
}

}  // namespace rfmap
