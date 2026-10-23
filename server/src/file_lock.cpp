#include "file_lock.hpp"

namespace rfmap {

std::unique_ptr<FileLock> ModuleGate::lockFile() const {
    if (lockFile_.empty()) return nullptr;
    std::filesystem::create_directories(lockFile_.parent_path());
    return std::make_unique<FileLock>(lockFile_, FileLock::Mode::Exclusive);
}

}  // namespace rfmap
