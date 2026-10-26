#include "profile_fit.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <string>

namespace fpv::altitude {
namespace {

constexpr double kEpsilonM = 1e-6;
/// Puts a dip under the floor, or a rise over the ceiling, ahead of any distance from the profile.
constexpr double kOutside = 1e6;
/// A waypoint whose removal adds no more climb than this, and keeps the line in the corridor, goes.
constexpr double kPruneRiseM = 0.5;

}  // namespace

ProfileFitter::ProfileFitter(const std::vector<CorridorPoint>& pts, const Route& route, const json& waypoints, double toleranceM)
    : pts_(pts), route_(route), waypoints_(waypoints), tolerance_(toleranceM) {}

std::size_t ProfileFitter::sampleAt(double s) const {
    const auto it = std::ranges::min_element(pts_, {}, [s](const CorridorPoint& p) { return std::abs(p.s - s); });
    return static_cast<std::size_t>(std::distance(pts_.begin(), it));
}

double ProfileFitter::reference(std::size_t i, double mslM) const {
    return std::ceil(route_.altitude().heightFor(mslM, pts_[i].groundMslM) * 10.0 - kEpsilonM) / 10.0;
}

double ProfileFitter::lineAt(std::size_t i, std::size_t a, double refA, std::size_t b, double refB) const {
    const double span = pts_[b].s - pts_[a].s;
    const double t = span > 0.0 ? (pts_[i].s - pts_[a].s) / span : 0.0;
    return route_.altitude().mslM(std::lerp(refA, refB, t), pts_[i].groundMslM);
}

bool ProfileFitter::outside(std::size_t i, double line, double profile) const {
    const double floor = pts_[i].floorMslM();
    const double ceiling = pts_[i].ceilingMslM;
    const bool under = profile >= floor - kEpsilonM && line < floor - kEpsilonM;
    // Where the floor is over the ceiling there is no corridor to keep under.
    const bool over = ceiling >= floor && profile <= ceiling + kEpsilonM && line > ceiling + kEpsilonM;
    return under || over;
}

double ProfileFitter::rise(const std::vector<double>& ref, const std::vector<std::size_t>& knots, std::size_t from, std::size_t to) const {
    double total = 0.0;
    for (std::size_t j = from; j < to; ++j) {
        const std::size_t a = knots[j];
        const std::size_t b = knots[j + 1];
        double prev = lineAt(a, a, ref[a], b, ref[b]);
        for (std::size_t i = a + 1; i <= b; ++i) {
            const double v = lineAt(i, a, ref[a], b, ref[b]);
            total += std::max(0.0, v - prev);
            prev = v;
        }
    }
    return total;
}

void ProfileFitter::prune(const std::vector<double>& profile, const std::vector<double>& ref, std::vector<std::size_t>& knots) const {
    std::size_t j = 1;
    while (j + 1 < knots.size()) {
        const std::size_t a = knots[j - 1];
        const std::size_t b = knots[j + 1];
        const bool keep = rise(ref, {a, b}, 0, 1) > rise(ref, knots, j - 1, j + 1) + kPruneRiseM ||
                          std::ranges::any_of(std::views::iota(a + 1, b), [&](std::size_t i) {
                              return outside(i, lineAt(i, a, ref[a], b, ref[b]), profile[i]);
                          });
        if (keep) {
            ++j;
        } else {
            knots.erase(knots.begin() + static_cast<std::ptrdiff_t>(j));
        }
    }
}

void ProfileFitter::split(const std::vector<double>& profile, std::vector<double>& ref, std::size_t a, std::size_t b,
                          std::vector<std::size_t>& knots) const {
    if (b <= a + 1) return;
    std::size_t worst = a;
    double score = tolerance_;
    for (std::size_t i = a + 1; i < b; ++i) {
        const double line = lineAt(i, a, ref[a], b, ref[b]);
        if (const double sc = std::abs(line - profile[i]) + (outside(i, line, profile[i]) ? kOutside : 0.0); sc > score) {
            worst = i;
            score = sc;
        }
    }
    if (worst == a) return;
    ref[worst] = reference(worst, profile[worst]);
    knots.push_back(worst);
    split(profile, ref, a, worst, knots);
    split(profile, ref, worst, b, knots);
}

FittedRoute ProfileFitter::fit(const std::vector<double>& profile) const {
    FittedRoute out;
    out.mslM.assign(pts_.size(), 0.0);
    std::vector ref(pts_.size(), 0.0);
    const std::string field(route_.altitude().waypointField());
    std::size_t count = 0;
    for (const Leg& leg : route_.legs()) {
        const std::size_t a = sampleAt(leg.startM);
        const std::size_t b = sampleAt(leg.startM + leg.lengthM);
        if (leg.index == 0) ref[a] = leg.from.heightM;
        ref[b] = leg.isReturn ? leg.to.heightM : reference(b, profile[b]);
        std::vector<std::size_t> knots;
        split(profile, ref, a, b, knots);
        std::ranges::sort(knots);
        knots.insert(knots.begin(), a);
        knots.push_back(b);
        prune(profile, ref, knots);
        for (std::size_t j = 1; j + 1 < knots.size(); ++j) {
            const std::size_t k = knots[j];
            out.waypoints.push_back({{"lat", pts_[k].p.position.lat}, {"lon", pts_[k].p.position.lon},
                                     {"label", std::format("C{}", ++count)}, {field, ref[k]}, {"heightSource", "manual"}});
        }
        out.added += knots.size() - 2;
        if (!leg.isReturn) {
            json w = waypoints_.at(leg.index);
            w[field] = ref[b];
            w["heightSource"] = "manual";
            out.waypoints.push_back(std::move(w));
        }
        for (std::size_t j = 0; j + 1 < knots.size(); ++j) {
            for (std::size_t i = knots[j]; i <= knots[j + 1]; ++i) out.mslM[i] = lineAt(i, knots[j], ref[knots[j]], knots[j + 1], ref[knots[j + 1]]);
        }
    }
    return out;
}

}  // namespace fpv::altitude
