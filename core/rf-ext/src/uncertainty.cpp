#include "rfext/uncertainty.hpp"

#include <rfcore/propagation.hpp>

#include <algorithm>
#include <cmath>

namespace rfext {
namespace {

rfcore::TerrainProfile shifted(const rfcore::TerrainProfile& p, double interior, double start, double end) {
    std::vector<rfcore::ProfileSample> s = p.samples();
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double sign = i == 0 ? start : (i + 1 == s.size() ? end : interior);
        s[i].groundMsl = s[i].groundMsl + rfcore::Metres(sign * s[i].uncertainty.value());
    }
    return rfcore::TerrainProfile(std::move(s), p.sourceName(), p.azimuthDeg(), p.midLatitudeDeg());
}

/// Widens [lo, hi] to the value of term `id` at every corner that has it.
void widenOverCorners(const std::vector<rfcore::LinkResult>& corners, const std::string& id, double& lo, double& hi) {
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
    switch (v) {
        case MarginBand::Verdict::Reliable: return "reliable";
        case MarginBand::Verdict::Marginal: return "marginal";
        case MarginBand::Verdict::Blocked: return "blocked";
        case MarginBand::Verdict::BlockedMayWork: return "blocked-may-work";
    }
    return "blocked";
}

UncertaintyAnalyzer::Extremes UncertaintyAnalyzer::knifeEdgeExtremes(double lo, double hi) {
    auto j = [](double v) { return rfcore::KnifeEdge::loss(v).value(); };
    Extremes e{std::min(j(lo), j(hi)), std::max(j(lo), j(hi))};
    // Turning points of J lie about 1/|ν| apart for ν < 0 (the Fresnel phase is πν²/2); step at a
    // tenth of that so none is skipped, then refine each bracketed one.
    auto refine = [&j](double a, double b, bool findMax) {
        const double g = (std::sqrt(5.0) - 1.0) / 2.0;
        double c = b - g * (b - a), d = a + g * (b - a);
        double fc = j(c), fd = j(d);
        for (int i = 0; i < 200 && b - a > 1e-15 * std::max(1.0, std::abs(a)); ++i) {
            if ((fc > fd) == findMax) {
                b = d; d = c; fd = fc; c = b - g * (b - a); fc = j(c);
            } else {
                a = c; c = d; fc = fd; d = a + g * (b - a); fd = j(d);
            }
        }
        return j((a + b) / 2.0);
    };
    double prev = lo, fPrev = j(lo);
    double x = lo, fx = fPrev;
    while (x < hi) {
        const double step = 0.1 / (std::abs(x) + 1.0);
        const double next = std::min(hi, x + step);
        const double fNext = j(next);
        if (x > lo) {
            if (fx >= fPrev && fx >= fNext) e.max = std::max(e.max, refine(prev, next, true));
            if (fx <= fPrev && fx <= fNext) e.min = std::min(e.min, refine(prev, next, false));
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
    double nuLo = nominal.diffraction.nu, nuHi = nominal.diffraction.nu;
    for (const auto& c : corners) {
        nuLo = std::min(nuLo, c.diffraction.nu);
        nuHi = std::max(nuHi, c.diffraction.nu);
    }
    const Extremes j = knifeEdgeExtremes(nuLo, nuHi);

    // Every other term: its own extremes over the corners (they depend on the end heights only
    // through the ray length and geometry, which also peak at corners).
    double worstLoss = 0.0, bestLoss = 0.0;
    for (std::size_t t = 0; t < nominal.terms.size(); ++t) {
        const std::string& id = nominal.terms[t].id;
        if (id == "diffraction") {
            worstLoss += j.max;
            bestLoss += j.min;
            continue;
        }
        double hi = nominal.terms[t].value.value(), lo = hi;
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
