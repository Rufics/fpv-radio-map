#pragma once

#include <fpv/search.hpp>

#include <optional>

namespace fpv::relay {

/// Budget of one relay position: pilot → relay, and relay → drone along the route.
struct Hops {
    WorstMargin first;   ///< pilot → relay (the relay is the air end)
    WorstMargin second;  ///< relay → drone, worst along the route
    [[nodiscard]] double score() const { return std::min(first.marginDb, second.marginDb); }
};

Hops evaluateRelay(const ILinkEvaluator& eval, const std::vector<std::string>& links, const End& pilot, const End& relay,
                   const std::vector<RoutePoint>& samples);

/// route.analyze: per-hop budget for the chosen relay, and the best relay spot in the drawn area.
json analyze(const json& request, const IWorld& world, const ILinkEvaluator& eval);

}  // namespace fpv::relay
