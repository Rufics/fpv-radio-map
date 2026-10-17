#pragma once

#include <mutex>
#include <string>

namespace rfmap {

/// Line-oriented log to stderr: "<UTC> <level> <source>: <message>". Containers collect stderr.
class Logger {
public:
    enum class Level { Debug, Info, Warn, Error };
    void write(Level level, const std::string& source, const std::string& message);
    void info(const std::string& source, const std::string& m) { write(Level::Info, source, m); }
    void warn(const std::string& source, const std::string& m) { write(Level::Warn, source, m); }
    void error(const std::string& source, const std::string& m) { write(Level::Error, source, m); }
    void setQuiet(bool q) { quiet_ = q; }

private:
    std::mutex mutex_;
    bool quiet_ = false;
};

}  // namespace rfmap
