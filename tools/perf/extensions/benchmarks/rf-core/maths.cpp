// Closed-form pieces of a link calculation; each runs once per profile point or per path.

#include "rfcore/fresnel_integrals.hpp"
#include "rfcore/propagation.hpp"

#include <benchmark/benchmark.h>

namespace {

// x in hundredths: 0.5 and 1.4 take the power series, 3 and 20 the continued fraction.
void BM_FresnelIntegrals(benchmark::State& state) {
    const double x = static_cast<double>(state.range(0)) / 100.0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(rfcore::FresnelIntegrals::evaluate(x));
    }
}
BENCHMARK(BM_FresnelIntegrals)->Arg(50)->Arg(140)->Arg(300)->Arg(2000);

// nu in hundredths, from a clear path (-1) to a deep shadow (+8).
void BM_KnifeEdgeLoss(benchmark::State& state) {
    const double nu = static_cast<double>(state.range(0)) / 100.0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(rfcore::KnifeEdge::loss(nu));
    }
}
BENCHMARK(BM_KnifeEdgeLoss)->Arg(-100)->Arg(0)->Arg(240)->Arg(800);

void BM_FreeSpaceLoss(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(rfcore::FreeSpaceLoss::compute(rfcore::Metres(12'000.0), rfcore::Hertz(5.8e9)));
    }
}
BENCHMARK(BM_FreeSpaceLoss);

}  // namespace
