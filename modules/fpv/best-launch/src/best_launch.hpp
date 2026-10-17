#pragma once

#include <fpv/search.hpp>

namespace fpv::launch {

/// route.analyze: the launch point inside the drawn area whose worst link margin along the route
/// is highest, compared with the current pilot position.
json analyze(const json& request, const IWorld& world, const ILinkEvaluator& eval);

}  // namespace fpv::launch
