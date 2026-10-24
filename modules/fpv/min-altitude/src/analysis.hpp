#pragma once

#include <fpv/clearance.hpp>
#include <fpv/route.hpp>

#include <functional>
#include <map>
#include <string>

namespace fpv::minalt {

using fpv::LinkInput;
using fpv::LinkInputs;

/// route.analyze: the lowest AGL along the route that keeps `fraction` of the first Fresnel zone
/// clear for each evaluated link, compared with the planned AGL.
json analyze(const json& request, const IWorld& world, const LinkInputs& links);

}  // namespace fpv::minalt
