#include "logger.hpp"

#include "common.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace rfmap {

void Logger::write(Level level, const std::string& source, const std::string& message) {
    if (quiet_ && level < Level::Warn) return;
    static constexpr std::array<std::string_view, 4> names = {"debug", "info", "WARN", "ERROR"};
    std::lock_guard lock(mutex_);
    std::cerr << isoUtc(Clock::now()) << ' ' << names.at(static_cast<std::size_t>(level)) << ' ' << source << ": "
              << message << '\n';
}

}  // namespace rfmap
