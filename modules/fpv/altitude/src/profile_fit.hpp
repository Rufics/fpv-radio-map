#pragma once

#include "corridor.hpp"

#include <cstddef>
#include <vector>

namespace fpv::altitude {

/// A profile turned into the mission's waypoints.
struct FittedRoute {
    json waypoints = json::array();  ///< the mission's waypoints with new heights, and the added ones
    std::size_t added = 0;
    std::vector<double> mslM;        ///< what these waypoints fly at every sample
};

/// Fits a height profile with the fewest waypoints: the mission interpolates heights linearly in
/// its own reference between waypoints, so points are added along a leg (at the sample that is
/// furthest off, until none is) wherever that line strays from the profile by more than the
/// tolerance, dips under the floor where the profile is over it, or rises over the ceiling where
/// the profile is under it. The existing waypoints keep
/// their places and take the profile's height; heights are rounded up to 0.1 m. Then every added
/// point whose removal keeps the line in the corridor without more climb is dropped: where the
/// profile only bends over a wavy floor on the way down, one straight line is as good.
class ProfileFitter {
public:
    ProfileFitter(const std::vector<CorridorPoint>& pts, const Route& route, const json& waypoints, double toleranceM);

    [[nodiscard]] FittedRoute fit(const std::vector<double>& profileMslM) const;

private:
    /// Height in the mission's reference at sample i for an MSL height, rounded up to 0.1 m.
    [[nodiscard]] double reference(std::size_t i, double mslM) const;
    /// MSL at sample i on the line between knots a and b, given their heights in the reference.
    [[nodiscard]] double lineAt(std::size_t i, std::size_t a, double refA, std::size_t b, double refB) const;
    /// Whether a line at sample i leaves the corridor where the profile is inside it.
    [[nodiscard]] bool outside(std::size_t i, double line, double profile) const;
    /// Total climb along the lines between knots[from] and knots[to].
    [[nodiscard]] double rise(const std::vector<double>& ref, const std::vector<std::size_t>& knots, std::size_t from, std::size_t to) const;
    /// Drops the knots (ends kept) whose line without them stays in the corridor and climbs no more.
    void prune(const std::vector<double>& profile, const std::vector<double>& ref, std::vector<std::size_t>& knots) const;
    /// Adds the knots strictly between a and b that keep the line within tolerance.
    void split(const std::vector<double>& profile, std::vector<double>& ref, std::size_t a, std::size_t b,
               std::vector<std::size_t>& knots) const;
    [[nodiscard]] std::size_t sampleAt(double s) const;

    const std::vector<CorridorPoint>& pts_;
    const Route& route_;
    const json& waypoints_;
    double tolerance_;
};

}  // namespace fpv::altitude
