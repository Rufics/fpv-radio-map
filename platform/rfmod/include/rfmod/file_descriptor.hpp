#pragma once

#include <unistd.h>

#include <utility>

namespace rfmod {

/// An open file descriptor closed with its owner. Move-only; -1 is "none".
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

private:
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};

}  // namespace rfmod
