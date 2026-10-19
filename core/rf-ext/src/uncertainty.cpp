#include "rfext/uncertainty.hpp"

#include <rfcore/propagation.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace rfext {
namespace {

/// Golden-section refinement of a turning point of the knife-edge loss J(ν).
class KnifeEdgeSearch {
public:
    static double loss(double nu) { return rfcore::KnifeEdge::loss(nu).value(); }

    /// J at the maximum (findMax) or minimum of J inside [a, b], which brackets one turning point.
    static double refine(double a, double b, bool findMax) {
        const double g = (std::sqrt(5.0) - 1.0) / 2.0;
        double c = std::lerp(b, a, g);
        double d = std::lerp(a, b, g);
        double fc = loss(c);
        double fd = loss(d);
        for (int i = 0; i < 200; ++i) {
            if (b - a <= 1e-15 * std::max(1.0, std::abs(a))) break;
            if ((fc > fd) == findMax) {
                b = d;
                d = c;
                fd = fc;
                c = std::lerp(b, a, g);
                fc = loss(c);
            } else {
                a = c;
                c = d;
                fc = fd;
                d = std::lerp(a, b, g);
                fd = loss(d);
            }
        }
        return loss((a + b) / 2.0);
    }
};

rfcore::TerrainProfile shifted(const rfcore::TerrainProfile& p, double interior, double start, double end) {
    std::vector<rfcore::ProfileSample> s = p.samples();
    for (std::size_t i = 0; i < s.size(); ++i) {
        double sign = interior;
        if (i == 0) {
            sign = start;
        } else if (i + 1 == s.size()) {
            sign = end;
        }
        s[i].groundMsl = s[i].groundMsl + rfcore::Metres(sign * s[i].uncertainty.value());
    }
    return rfcore::TerrainProfile(std::move(s), p.sourceName(), p.azimuthDeg(), p.midLatitudeDeg());
}

/// Widens [lo, hi] to the value of term `id` at every corner that has it.
void widenOverCorners(const std::vector<rfcore::LinkResult>& corners, std::string_view id, double& lo, double& hi) {
    for (const auto& c : corners) {
        for (const auto& term : c.terms) {
            if (term.id != id) continue;
            hi = std::max(hi, term.value.value());
            lo = std::min(lo, term.value.value());
        }
    }
}

}  // namespace

std::string toString(MarginBand::Verdict v) {
    using enum MarginBand::Verdict;
    switch (v) {
        case Reliable: return "reliable";
        case Marginal: return "marginal";
        case Blocked: return "blocked";
        case BlockedMayWork: return "blocked-may-work";
    }
    return "blocked";
}

UncertaintyAnalyzer::Extremes UncertaintyAnalyzer::knifeEdgeExtremes(double lo, double hi) {
    const auto j = KnifeEdgeSearch::loss;
    Extremes e{std::min(j(lo), j(hi)), std::max(j(lo), j(hi))};
    // Turning points of J lie about 1/|ν| apart for ν < 0 (the Fresnel phase is πν²/2); step at a
    // tenth of that so none is skipped, then refine each bracketed one.
    double prev = lo;
    double fPrev = j(lo);
    double x = lo;
    double fx = fPrev;
    while (x < hi) {
        const double step = 0.1 / (std::abs(x) + 1.0);
        const double next = std::min(hi, x + step);
        const double fNext = j(next);
        if (x > lo) {
            if (fx >= fPrev && fx >= fNext) e.max = std::max(e.max, KnifeEdgeSearch::refine(prev, next, true));
            if (fx <= fPrev && fx <= fNext) e.min = std::min(e.min, KnifeEdgeSearch::refine(prev, next, false));
        }
        prev = x;
        fPrev = fx;
        x = next;
        fx = fNext;
    }
    return e;
}

MarginBand UncertaintyAnalyzer::analyze(const rfcore::TerrainProfile& profile, rfcore::Hertz frequency,
                                        const rfcore::TransmitterEnd& tx, const rfcore::ReceiverEnd& rx) const {
    const rfcore::LinkResult nominal = evaluator_.evaluate(profile, frequency, tx, rx);
    std::vector<rfcore::LinkResult> corners;
    for (double interior : {1.0, -1.0}) {
        for (double a : {1.0, -1.0}) {
            for (double b : {1.0, -1.0}) {
                corners.push_back(evaluator_.evaluate(shifted(profile, interior, a, b), frequency, tx, rx));
            }
        }
    }
    double nuLo = nominal.diffraction.nu;
    double nuHi = nominal.diffraction.nu;
    for (const auto& c : corners) {
        nuLo = std::min(nuLo, c.diffraction.nu);
        nuHi = std::max(nuHi, c.diffraction.nu);
    }
    const Extremes j = knifeEdgeExtremes(nuLo, nuHi);

    // Every other term: its own extremes over the corners (they depend on the end heights only
    // through the ray length and geometry, which also peak at corners).
    double worstLoss = 0.0;
    double bestLoss = 0.0;
    for (const auto& t : nominal.terms) {
        const std::string& id = t.id;
        if (id == "diffraction") {
            worstLoss += j.max;
            bestLoss += j.min;
            continue;
        }
        double hi = t.value.value();
        double lo = hi;
        widenOverCorners(corners, id, lo, hi);
        worstLoss += hi;
        bestLoss += lo;
    }
    double nominalLoss = 0.0;
    for (const auto& term : nominal.terms) nominalLoss += term.value.value();
    const double m0 = nominal.margin.value();
    const double worst = m0 - (worstLoss - nominalLoss);
    const double best = m0 + (nominalLoss - bestLoss);

    MarginBand band{rfcore::Decibel(m0), rfcore::Decibel(worst), rfcore::Decibel(best), MarginBand::Verdict::Reliable};
    if (worst >= 0.0) {
        band.verdict = MarginBand::Verdict::Reliable;
    } else if (m0 >= 0.0) {
        band.verdict = MarginBand::Verdict::Marginal;
    } else if (best >= 0.0) {
        band.verdict = MarginBand::Verdict::BlockedMayWork;
    } else {
        band.verdict = MarginBand::Verdict::Blocked;
    }
    return band;
}

}  // namespace rfext
