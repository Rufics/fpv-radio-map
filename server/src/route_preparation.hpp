#pragma once

#include "module_runner.hpp"

namespace rfmap {

/// The step before the route calculation: route.prepare on the opted-in route.preparation
/// modules, which give heights to the waypoints that leave theirs to the server (no height in
/// the mission's reference, or heightSource "auto"). A manual height is never changed. A module
/// that fails is listed in result.moduleFailures and the calculation goes on without it. With
/// every height set by hand, no module is asked.
class RoutePreparation {
public:
    explicit RoutePreparation(const ModuleRunner& modules) : modules_(modules) {}

    /// The mission with the modules' heights set, each listed in result.autoHeights. The
    /// modules' reasons go to `reasons` (with their module), for once the legs exist. Throws
    /// HttpError 422 when a waypoint is still without a height.
    [[nodiscard]] json prepare(const json& mission, const CalcContext& ctx, json& result, json& reasons) const;

    /// "altM" for a launch-referenced mission, "aglM" otherwise.
    [[nodiscard]] static std::string heightField(const json& mission);
    /// Whether the server is to set this waypoint's height.
    [[nodiscard]] static bool wantsHeight(const json& waypoint, const std::string& field);

private:
    /// Sets the heights of one module's answer that are still wanted, listing each in `heights`;
    /// false for a malformed answer.
    static bool apply(json& mission, const json& answer, const std::string& moduleId, json& heights,
                      std::vector<bool>& done);

    const ModuleRunner& modules_;
};

}  // namespace rfmap
