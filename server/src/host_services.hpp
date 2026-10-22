#pragma once

#include "host_methods.hpp"
#include "invocation_scope.hpp"

#include <rfmod/host.hpp>

namespace rfmap {

class Logger;

/// The host side of a module's calls, see docs/modules/developer-guide.md section 6. It answers
/// only inside an InvocationScope: the request the module is working for says which services,
/// time and forecast the call sees, and the scope's budget bounds what a module can ask for.
class ServerHostServices final : public rfmod::IHostServices {
public:
    explicit ServerHostServices(Logger& log) : log_(log) {}

    json call(const std::string& moduleId, const std::string& method, const json& request) override;
    void log(const std::string& moduleId, int level, const std::string& message) override;

    [[nodiscard]] const HostMethodTable& methods() const { return methods_; }

private:
    HostMethodTable methods_;
    Logger& log_;
};

}  // namespace rfmap
