#include "host_services.hpp"

#include "logger.hpp"

#include <rfmod/abi.h>

namespace rfmap {

namespace {

Logger::Level loggerLevel(int level) {
    using enum Logger::Level;
    if (level >= RFMOD_LOG_ERROR) return Error;
    if (level >= RFMOD_LOG_WARN) return Warn;
    return Info;
}

}  // namespace

void ServerHostServices::log(const std::string& moduleId, int level, const std::string& message) {
    log_.write(loggerLevel(level), "module " + moduleId, message);
}

json ServerHostServices::call(const std::string& moduleId, const std::string& method, const json& request) {
    const IHostMethod* m = methods_.find(method);
    if (!m) throw HostCallError("unknown host method " + method);
    const InvocationScope* scope = InvocationScope::current();
    if (!scope) {
        throw HostCallError(moduleId + " called " + method +
                            " outside a request: host calls are available only while the server evaluates one");
    }
    const InvocationScope::HostCall counted(*scope);
    return m->call(HostRequest(request), *scope);
}

}  // namespace rfmap
