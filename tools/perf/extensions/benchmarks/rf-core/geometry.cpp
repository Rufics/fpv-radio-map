// The per-path work that grows with the path length: geodesics, the plane geometry and the
// Bullington construction. Arguments are profile samples at a 30 m step (3 km to 50 km).

#include "terrain.hpp"

#include "rfcore/diffraction.hpp"
#include "rfcore/earth_model.hpp"
#include "rfcore/geodesy.hpp"
#include "rfcore/path_geometry.hpp"
#include "rfcore/propagation.hpp"

#include <benchmark/benchmark.h>

namespace {

const rfcore::GeoPoint kStart{52.2297, 21.0122};
const rfcore::GeoPoint kEnd{52.3100, 21.2400};

void BM_GeodesyInverse(benchmark::State& state) {
    const rfcore::Geodesy geodesy;
    for (auto _ : state) {
        benchmark::DoNotOptimize(geodesy.inverse(kStart, kEnd));
    }
}
BENCHMARK(BM_GeodesyInverse);

void BM_GeodesySample(benchmark::State& state) {
    const rfcore::Geodesy geodesy;
    for (auto _ : state) {
        benchmark::DoNotOptimize(geodesy.sample(kStart, kEnd, rfcore::Metres(rfperf::kStepMetres)));
    }
}
BENCHMARK(BM_GeodesySample);

void BM_PathGeometry(benchmark::State& state) {
    const auto profile = rfperf::rollingProfile(static_cast<std::size_t>(state.range(0)));
    const rfcore::EarthModel earth(4.0 / 3.0);
    for (auto _ : state) {
        const rfcore::PathGeometry geometry(profile, earth, rfcore::Metres(30.0), rfcore::Metres(2.0));
        benchmark::DoNotOptimize(geometry.tops().data());
    }
    state.SetComplexityN(state.range(0));
}
BENCHMARK(BM_PathGeometry)->Arg(100)->Arg(400)->Arg(1700)->Complexity();

void BM_BullingtonDiffraction(benchmark::State& state) {
    const auto profile = rfperf::rollingProfile(static_cast<std::size_t>(state.range(0)));
    const rfcore::EarthModel earth(4.0 / 3.0);
    const rfcore::PathGeometry geometry(profile, earth, rfcore::Metres(30.0), rfcore::Metres(2.0));
    const rfcore::BullingtonDiffraction diffraction;
    const rfcore::Metres lambda = rfcore::wavelength(rfcore::Hertz(5.8e9));
    for (auto _ : state) {
        benchmark::DoNotOptimize(diffraction.compute(geometry, lambda));
    }
    state.SetComplexityN(state.range(0));
}
BENCHMARK(BM_BullingtonDiffraction)->Arg(100)->Arg(400)->Arg(1700)->Complexity();

}  // namespace
