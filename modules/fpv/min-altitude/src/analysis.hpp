#pragma once

#include "clearance.hpp"

#include <fpv/route.hpp>

#include <map>
#include <string>

namespace fpv::minalt {

struct LinkInput {
    rfcore::Hertz frequency;
    std::string radioName;
};

/// route.analyze: the lowest AGL along the route that keeps `fraction` of the first Fresnel zone
/// clear for each evaluated link, compared with the planned AGL.
json analyze(const json& request, const IWorld& world, const std::map<std::string, LinkInput>& links);

}  // namespace fpv::minalt
