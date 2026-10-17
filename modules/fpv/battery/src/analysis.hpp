#pragma once

#include "energy_model.hpp"

namespace fpv::battery {

/// route.analyze for given library items; `world` answers geodesy and terrain.
json analyze(const json& request, const IWorld& world, const json& drone, const json& battery);

}  // namespace fpv::battery
