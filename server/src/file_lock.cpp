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
    std::unique_ptr<FileLock> file;
    if (!lockFile_.empty()) {
        std::filesystem::create_directories(lockFile_.parent_path());
        file = std::make_unique<FileLock>(lockFile_, FileLock::Mode::Exclusive);
    }
    mutex_.lock();
    held_ = std::move(file);
}

void ModuleGate::unlock() {
    const std::unique_ptr<FileLock> file = std::move(held_);
    mutex_.unlock();
}

}  // namespace rfmap
