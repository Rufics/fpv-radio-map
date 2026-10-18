#include "rfext/uncertainty.hpp"
#include "test_profiles.hpp"

#include <rfcore/diffraction.hpp>
#include <rfcore/earth_model.hpp>
#include <rfcore/propagation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace rfext;
using namespace rfcore;

namespace {

TransmitterEnd txEnd(double agl) {
    TransmitterEnd tx;
    tx.antennaAgl = Metres(agl);
    tx.antennaGain = DecibelIsotropic(2.0);
    tx.feederLoss = Decibel(0.5);
    tx.power = DecibelMilliwatt(27.0);
    return tx;
}

ReceiverEnd rxEnd(double agl) {
    ReceiverEnd rx;
    rx.antennaAgl = Metres(agl);
    rx.antennaGain = DecibelIsotropic(2.0);
    rx.feederLoss = Decibel(0.5);
    rx.sensitivity = DecibelMilliwatt(-105.0);
    rx.requiredMargin = Decibel(10.0);
    return rx;
}

/// A deterministic quasi-Monte Carlo sweep (a Kronecker sequence): point n of dimension k is
/// frac(n * sqrt(q_k)), q_k the k-th integer that is not a perfect square. Every dimension fills
/// [0, 1) evenly and the dimensions do not line up, so it stands in for independent uniform draws,
/// and every run checks the same points.
class Sweep {
public:
    explicit Sweep(int dimensions) {
        for (int q = 2; static_cast<int>(steps_.size()) < dimensions; ++q) {
            const double root = std::sqrt(static_cast<double>(q));
            if (root != std::floor(root)) steps_.push_back(root - std::floor(root));
        }
    }
    /// In [0, 1); n counts from 1.
    [[nodiscard]] double at(long n, int dimension) const {
        const double x = static_cast<double>(n) * steps_.at(static_cast<std::size_t>(dimension));
        return x - std::floor(x);
    }

private:
    std::vector<double> steps_;
};

TerrainProfile hills(const Sweep& sweep, long n, double length, double step) {
    std::vector<std::pair<double, double>> pts;
    const double a = 60.0 * sweep.at(n, 0);
    const double b = 60.0 * sweep.at(n, 1);
    const double c = 60.0 * sweep.at(n, 2);
    for (std::size_t i = 0; static_cast<double>(i) * step <= length; ++i) {
        const double d = static_cast<double>(i) * step;
        const double x = d / length;
        pts.emplace_back(d, 150.0 + a * std::sin(3.1 * x) + b * std::sin(7.3 * x + 1.0) + c * x * x);
    }
    return test::makeProfile(pts);
}

}  // namespace

TEST(Uncertainty, KnifeEdgeExtremesFindTheRipple) {
    // J(ν) has its deepest negative value (about -1.4 dB, a gain) near ν = -1.2.
    const auto e = UncertaintyAnalyzer::knifeEdgeExtremes(-3.0, 0.0);
    double brute = 1e9;
    for (int i = 0; i <= 300000; ++i) brute = std::min(brute, KnifeEdge::loss(-3.0 + 3.0 * i / 300000.0).value());
    EXPECT_LE(e.min, brute + 1e-12);
    EXPECT_GT(e.min, brute - 1e-9);
    EXPECT_NEAR(e.max, KnifeEdge::loss(0.0).value(), 1e-12);  // J rises with ν beyond the ripple
}

TEST(Uncertainty, VerdictFollowsTheBand) {
    LinkEvaluator eval(EarthModel(4.0 / 3.0), BullingtonDiffraction());
    const UncertaintyAnalyzer an(eval);
    // Flat open path: margin far above zero at both ends of the band.
    const auto flat = test::makeProfile({{0, 100}, {1000, 100}, {2000, 100}, {3000, 100}});
    const auto band = an.analyze(flat, Hertz(2.44e9), txEnd(2.0), rxEnd(100.0));
    EXPECT_EQ(band.verdict, MarginBand::Verdict::Reliable);
    EXPECT_LE(band.worst.value(), band.nominal.value());
    EXPECT_GE(band.best.value(), band.nominal.value());
}

TEST(Uncertainty, BandContainsEveryPerturbationWithinTheStatedError) {
    // The band is exact only if the margin is monotone in each sample height. Check that against
    // perturbations of every sample spread evenly over the stated error, on varied hilly paths.
    constexpr int kSamples = 201;  // 6000 m at 30 m steps
    const Sweep sweep(3 + kSamples);
    LinkEvaluator eval(EarthModel(4.0 / 3.0), BullingtonDiffraction());
    const UncertaintyAnalyzer an(eval);
    int checked = 0;
    for (int path = 0; path < 100; ++path) {
        const TerrainProfile p = hills(sweep, path + 1, 6000.0, 30.0);
        ASSERT_EQ(p.samples().size(), static_cast<std::size_t>(kSamples));
        const auto tx = txEnd(1.5);
        const auto rx = rxEnd(5.0 + 2.5 * path);
        const auto band = an.analyze(p, Hertz(5.8e9), tx, rx);
        for (int trial = 0; trial < 40; ++trial) {
            std::vector<ProfileSample> s = p.samples();
            const long n = 40L * path + trial + 1;
            for (std::size_t k = 0; k < s.size(); ++k) {
                const double unit = 2.0 * sweep.at(n, 3 + static_cast<int>(k)) - 1.0;
                s[k].groundMsl = s[k].groundMsl + Metres(unit * s[k].uncertainty.value());
            }
            const TerrainProfile q(std::move(s), p.sourceName(), p.azimuthDeg(), p.midLatitudeDeg());
            const auto r = eval.evaluate(q, Hertz(5.8e9), tx, rx);
            const double m = r.margin.value();
            EXPECT_GE(m, band.worst.value() - 1e-9) << "path " << path;
            EXPECT_LE(m, band.best.value() + 1e-9) << "path " << path;
            ++checked;
        }
    }
    EXPECT_EQ(checked, 4000);
}
