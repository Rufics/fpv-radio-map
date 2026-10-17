#include "logger.hpp"

#include "common.hpp"

#include <iostream>

namespace rfmap {

void Logger::write(Level level, const std::string& source, const std::string& message) {
    if (quiet_ && level < Level::Warn) return;
    static const char* names[] = {"debug", "info", "WARN", "ERROR"};
    std::lock_guard lock(mutex_);
    std::cerr << isoUtc(Clock::now()) << ' ' << names[static_cast<int>(level)] << ' ' << source << ": " << message
              << std::endl;
}

}  // namespace rfmap
