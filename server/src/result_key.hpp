#pragma once

#include "builtins.hpp"
#include "link_service.hpp"
#include "module_runner.hpp"

#include <string>

namespace rfmap {

/// The library items a calculation used, each version listed once (both links often share
/// antennas).
class LibraryRefList {
public:
    void add(const json& ref);
    void addAll(const std::vector<LibraryRef>& refs);
    [[nodiscard]] const json& toJson() const { return refs_; }

private:
    json refs_ = json::array();
};

/// The hash every result is cached under: the request, the library items, the time and the
/// environment (terrain files, built-in switches, module versions, server version, forecast).
class ResultKeyBuilder {
public:
    ResultKeyBuilder(const LinkService& links, const BuiltinModules& builtins, const ModuleRunner& modules);

    [[nodiscard]] json environment(const CalcContext& ctx) const;
    /// `inputs` carries the kind and the request; time and environment are added here.
    [[nodiscard]] std::string hash(json inputs, const LibraryRefList& refs, const CalcContext& ctx) const;

private:
    const LinkService& links_;
    const BuiltinModules& builtins_;
    const ModuleRunner& modules_;
};

}  // namespace rfmap
