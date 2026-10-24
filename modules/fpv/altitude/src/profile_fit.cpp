#include "profile_fit.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace fpv::altitude {
namespace {

constexpr double kEpsilonM = 1e-6;
/// Puts a dip under the floor ahead of any distance from the profile.
constexpr double kUnderFloor = 1e6;

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

void ProfileFitter::split(const std::vector<double>& profile, std::vector<double>& ref, std::size_t a, std::size_t b,
                          std::vector<std::size_t>& knots) const {
    if (b <= a + 1) return;
    std::size_t worst = a;
    double score = tolerance_;
    for (std::size_t i = a + 1; i < b; ++i) {
        const double line = lineAt(i, a, ref[a], b, ref[b]);
        const double floor = pts_[i].floorMslM();
        const bool under = profile[i] >= floor - kEpsilonM && line < floor - kEpsilonM;
        if (const double sc = std::abs(line - profile[i]) + (under ? kUnderFloor : 0.0); sc > score) {
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
    std::vector<double> ref(pts_.size(), 0.0);
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
        for (const std::size_t k : knots) {
            out.waypoints.push_back({{"lat", pts_[k].p.position.lat}, {"lon", pts_[k].p.position.lon},
                                     {"label", "C" + std::to_string(++count)}, {field, ref[k]}, {"heightSource", "manual"}});
        }
        out.added += knots.size();
        if (!leg.isReturn) {
            json w = waypoints_.at(leg.index);
            w[field] = ref[b];
            w["heightSource"] = "manual";
            out.waypoints.push_back(std::move(w));
        }
        knots.insert(knots.begin(), a);
        knots.push_back(b);
        for (std::size_t j = 0; j + 1 < knots.size(); ++j) {
            for (std::size_t i = knots[j]; i <= knots[j + 1]; ++i) out.mslM[i] = lineAt(i, knots[j], ref[knots[j]], knots[j + 1], ref[knots[j + 1]]);
        }
    }
    return out;
}

}  // namespace fpv::altitude
