#pragma once

#include <rfmod/host.hpp>

#include <exception>
#include <format>
#include <string>

namespace rfmap {

/// Runs one call into a module. A module can fail in any way; the failure is reported in the
/// result, never raised, so a calculation goes on without the module.
class ModuleCall {
public:
    /// How `call` failed, or "" when it did not.
    template <class Call>
    [[nodiscard]] static std::string failure(const Call& call) {
        try {
            call();
        } catch (const rfmod::ModuleError& e) {
            return std::format("{}: {}", e.code(), e.what());
        } catch (const std::exception& e) {  // NOSONAR(cpp:S1181): a module failure is listed, never fails the calculation
            return e.what();
        }
        return {};
    }
};

}  // namespace rfmap
