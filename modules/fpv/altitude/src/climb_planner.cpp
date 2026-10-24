#include "climb_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fpv::altitude {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEpsilonM = 1e-6;
/// Per sample and metre over the floor: prefers the lower of two profiles with the same climb,
/// and stays far below one grid step over a whole route.
constexpr double kLowWeight = 1e-6;

/// The heights each sample may take, and the least climb to each with the height it came from.
struct Lattice {
    std::vector<std::vector<double>> h;
    std::vector<std::vector<double>> cost;
    std::vector<std::vector<std::size_t>> from;
};

/// One step of the search: the least climb to every height of sample i + 1 from sample i.
void relax(Lattice& l, std::size_t i, const Segment& seg, double floor, bool rates) {
    const std::vector<double>& here = l.h[i];
    const std::vector<double>& next = l.h[i + 1];
    l.cost[i + 1].assign(next.size(), kInf);
    l.from[i + 1].assign(next.size(), 0);
    for (std::size_t j = 0; j < next.size(); ++j) {
        const double low = kLowWeight * std::max(0.0, next[j] - floor);
        for (std::size_t k = 0; k < here.size(); ++k) {
            const double d = next[j] - here[k];
            const bool reachable = std::isfinite(l.cost[i][k]) && (!rates || (d <= seg.maxRiseM + kEpsilonM && -d <= seg.maxDropM + kEpsilonM));
            if (const double c = l.cost[i][k] + std::max(0.0, d) + low; reachable && c < l.cost[i + 1][j]) {
                l.cost[i + 1][j] = c;
                l.from[i + 1][j] = k;
            }
        }
    }
}

/// Time over a stretch: flying it, or climbing or descending its height where that takes longer.
double stretchTime(const Segment& seg, double ds, double rise) {
    double t = seg.groundSpeedMps > 0.0 ? ds / seg.groundSpeedMps : 0.0;
    if (seg.climbMps > 0.0) t = std::max(t, rise / seg.climbMps);
    if (seg.descentMps > 0.0) t = std::max(t, -rise / seg.descentMps);
    return t;
}

}  // namespace

ClimbPlanner::ClimbPlanner(const std::vector<CorridorPoint>& pts, std::vector<Segment> segments, bool returns, double heightStepM)
    : pts_(pts), seg_(std::move(segments)), returns_(returns), step_(heightStepM),
      start_(pts.front().plannedMslM), end_(pts.back().plannedMslM) {}

double ClimbPlanner::highestFloor() const {
    double top = start_;
    const std::size_t last = returns_ ? pts_.size() - 1 : pts_.size();
    for (std::size_t i = 1; i < last; ++i) top = std::max(top, pts_[i].floorMslM());
    return top;
}

std::optional<std::vector<double>> ClimbPlanner::search(bool rates) const {
    const std::size_t n = pts_.size();
    double lo = std::min(start_, end_);
    double hi = std::max(start_, end_);
    for (const CorridorPoint& p : pts_) {
        lo = std::min(lo, p.floorMslM());
        hi = std::max({hi, p.ceilingMslM, p.floorMslM()});
    }
    // The grid runs through the highest floor, so flying level over everything is on it.
    const double top = highestFloor();
    const double base = top - std::ceil((top - lo) / step_ - kEpsilonM) * step_;
    const auto levels = static_cast<std::size_t>(std::ceil((hi - base) / step_)) + 1;

    // The heights a sample may take: on the grid between its floor and ceiling (the floor itself
    // where no grid height fits, as where the floor is above the ceiling), and the plan's own at
    // fixed ends.
    Lattice l{std::vector<std::vector<double>>(n), std::vector<std::vector<double>>(n), std::vector<std::vector<std::size_t>>(n)};
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0 || (returns_ && i == n - 1)) {
            l.h[i] = {i == 0 ? start_ : end_};
            continue;
        }
        const double floor = pts_[i].floorMslM();
        const double cap = std::max(pts_[i].ceilingMslM, floor);
        for (std::size_t k = 0; k < levels; ++k) {
            if (const double v = base + static_cast<double>(k) * step_; v >= floor - kEpsilonM && v <= cap + kEpsilonM) l.h[i].push_back(v);
        }
        if (l.h[i].empty()) l.h[i] = {floor};
    }

    // Least climb to each height of each sample; the graph only goes forward along the route.
    l.cost[0] = {0.0};
    l.from[0] = {0};
    for (std::size_t i = 0; i + 1 < n; ++i) relax(l, i, seg_[i], pts_[i + 1].floorMslM(), rates);
    const auto best = std::ranges::min_element(l.cost.back());
    if (!std::isfinite(*best)) return std::nullopt;
    std::vector<double> out(n);
    auto k = static_cast<std::size_t>(std::distance(l.cost.back().begin(), best));
    for (std::size_t i = n; i-- > 0;) {
        out[i] = l.h[i][k];
        k = l.from[i][k];
    }
    return out;
}

bool ClimbPlanner::withinRates(const std::vector<double>& mslM) const {
    for (std::size_t i = 0; i + 1 < mslM.size(); ++i) {
        const double d = mslM[i + 1] - mslM[i];
        if (d > seg_[i].maxRiseM + kEpsilonM || -d > seg_[i].maxDropM + kEpsilonM) return false;
    }
    return true;
}

ClimbProfile ClimbPlanner::energyOptimal() const {
    if (auto p = search(true)) return {"energy", std::move(*p), true};
    // Without the rates every height is reachable from the pilot, so this always finds one.
    return {"energy", search(false).value_or(std::vector<double>(pts_.size(), highestFloor())), false};
}

ClimbProfile ClimbPlanner::overLaunch() const {
    std::vector<double> out(pts_.size(), highestFloor());
    out.front() = start_;
    if (returns_) out.back() = end_;
    const bool ok = withinRates(out);
    return {"over-launch", std::move(out), ok};
}

ClimbProfile ClimbPlanner::gradual() const {
    const std::size_t n = pts_.size();
    // Past the take-off zone: in it the drone climbs (and lands) vertically to the floor at its edge.
    const auto open = [this](std::size_t i) { return !pts_[i].nearPilot; };
    std::size_t first = 1;
    while (first < n && !open(first)) ++first;
    std::size_t last = returns_ ? n - 2 : n - 1;
    while (last > first && !open(last)) --last;
    if (first >= n || first > last) return overLaunch();

    const double top = highestFloor();
    std::size_t peak = first;
    std::size_t peakEnd = first;
    for (std::size_t i = first; i <= last; ++i) {
        if (pts_[i].floorMslM() < top - kEpsilonM) continue;
        if (peakEnd == first && pts_[peak].floorMslM() < top - kEpsilonM) peak = i;
        peakEnd = i;
    }
    if (pts_[peak].floorMslM() < top - kEpsilonM) peak = peakEnd;

    // The gentlest straight climb from the zone's edge that clears every floor up to the peak,
    // and on a return the gentlest straight descent from the peak to the zone's edge at the end.
    const double base = std::max(start_, pts_[first].floorMslM());
    double up = 0.0;
    for (std::size_t i = first + 1; i <= peak; ++i) up = std::max(up, (pts_[i].floorMslM() - base) / (pts_[i].s - pts_[first].s));
    const double land = std::max(end_, pts_[last].floorMslM());
    double down = 0.0;
    for (std::size_t i = peakEnd; returns_ && i < last; ++i) down = std::max(down, (pts_[i].floorMslM() - land) / (pts_[last].s - pts_[i].s));

    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i < first) {
            out[i] = std::max(base, pts_[i].floorMslM());
        } else if (i > last) {
            out[i] = std::max(land, pts_[i].floorMslM());
        } else {
            double v = std::min(top, base + up * (pts_[i].s - pts_[first].s));
            if (returns_) v = std::min(v, land + down * (pts_[last].s - pts_[i].s));
            out[i] = v;
        }
    }
    out.front() = start_;
    if (returns_) out.back() = end_;
    const bool ok = withinRates(out);
    return {"gradual", std::move(out), ok};
}

ProfileStats ClimbPlanner::stats(const std::vector<double>& mslM) const {
    ProfileStats st;
    st.minClearanceM = kInf;
    st.overCeilingM = 0.0;
    for (std::size_t i = 0; i < mslM.size(); ++i) {
        const CorridorPoint& p = pts_[i];
        if (!p.nearPilot && mslM[i] - p.floorMslM() < st.minClearanceM) {
            st.minClearanceM = mslM[i] - p.floorMslM();
            st.minClearanceAtM = p.s;
        }
        // Where the floor is over the ceiling, every profile has to be over it.
        if (p.ceilingMslM >= p.floorMslM()) st.overCeilingM = std::max(st.overCeilingM, mslM[i] - p.ceilingMslM);
        if (i + 1 == mslM.size()) break;
        const double d = mslM[i + 1] - mslM[i];
        const double ds = pts_[i + 1].s - p.s;
        const Segment& seg = seg_[i];
        st.riseM += std::max(0.0, d);
        st.timeS += stretchTime(seg, ds, d);
        if (ds > 0.0 && std::isfinite(seg.maxRiseM)) {
            if (const double rate = d / ds * seg.groundSpeedMps; rate > st.steepestClimbMps) {
                st.steepestClimbMps = rate;
                st.steepestClimbAtM = p.s;
            }
        }
    }
    if (!std::isfinite(st.minClearanceM)) st.minClearanceM = 0.0;
    return st;
}

}  // namespace fpv::altitude
