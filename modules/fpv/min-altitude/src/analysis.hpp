#pragma once

#include "clearance.hpp"

#include <fpv/route.hpp>

#include <functional>
#include <map>
#include <string>

namespace fpv::minalt {

struct LinkInput {
    rfcore::Hertz frequency;
    std::string radioName;
};

/// Radio of each evaluated link, by link kind.
using LinkInputs = std::map<std::string, LinkInput, std::less<>>;

/// route.analyze: the lowest AGL along the route that keeps `fraction` of the first Fresnel zone
/// clear for each evaluated link, compared with the planned AGL.
json analyze(const json& request, const IWorld& world, const LinkInputs& links);

}  // namespace fpv::minalt
