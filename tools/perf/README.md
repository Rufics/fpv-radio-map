# Performance tools

Tooling for the load and performance tests of the C++ code (core, platform, FPV modules and the
server). So far there are the terrain dataset builder, the catalog of descriptions and
extensions, and the micro suite (Google Benchmark) with its history and baseline; the k6
scenarios, monitoring and reports follow. CI runs only the unit tests of
these tools, which include a `catalog check` of the repository catalog; the perf runs themselves
are not part of CI.

## Catalog (`perf.py`)

Everything a perf run uses is an item of the catalog: a folder with a `manifest.json`. New
personas, scenarios or collectors are new folders; the core code does not change.

| kind | folder | group | what it is |
|---|---|---|---|
| `fixture` | `fixtures/<id>/` | description | shared request data: points, request bodies, link set-ups |
| `persona` | `personas/<id>/` | description | one kind of user: sign-in, steps, pauses |
| `scenario` | `scenarios/<id>/` | description | which personas, how much load, how long, thresholds |
| `collector` | `extensions/collectors/<id>/` | extension | Python code that reads metrics from one tool |
| `reporter` | `extensions/reporters/<id>/` | extension | Python code that turns results into a report |
| `benchmark` | `extensions/benchmarks/<id>/` | extension | Google Benchmark sources for one C++ component |

Descriptions are JSON only; extensions also carry code. Each kind is defined by
`schemas/<kind>.schema.json`: its `x-perfkit` block says where items live, which fields refer to
other items (`"fixtures": ["warsaw-points"]`, `"body": "link-setups:control.json"`) and which name
files that must exist (`"entry": "collector.py:K6SummaryCollector"`). A new kind is a new schema.
The id of an item must equal its folder name.

```bash
python3 -I tools/perf/perf.py list [--kind persona]
python3 -I tools/perf/perf.py catalog check
python3 -I tools/perf/perf.py catalog lock
```

`--catalog-dir DIR` (before the command) reads another catalog folder; the default is
`tools/perf`.

`catalog check` validates every manifest against its schema, checks that referenced items and
files exist, and compares the catalog with `catalog.lock.json`. The lock file holds the version
and a SHA-256 of the content of every item, so an item cannot change without a new version:
results are only compared between runs with the same major versions. After a change, raise
`version` in the manifest (major: results no longer comparable, minor: comparable but the load
changed a little, patch: the numbers do not change) and run `catalog lock`. It refuses while an
item changed without a higher version.

Exit codes: 0 on success, 1 when the catalog has problems or a run regressed, 2 on a usage error.

## Micro suite (`perf.py run --suite micro`)

Google Benchmark binaries for the hot C++ code, one `benchmark` extension per component:

| benchmark | what it measures |
|---|---|
| `rf-core` | Fresnel integrals, knife edge, free-space loss, geodesics, path geometry and Bullington diffraction for 3, 12 and 50 km profiles at 30 m |
| `result-cache` | input hashing, memory hits from 1 to 8 threads, promotion from the table tier, eviction |

```bash
sudo apt-get install libbenchmark-dev     # plus the packages of .github/actions/cpp-deps
python3 -I tools/perf/perf.py run --suite micro [--only rf-core] [--repetitions 10]
```

`run` configures a Release build in `build-perf/` with `RFMAP_BUILD_BENCHMARKS=ON`, builds the
benchmark targets and runs each binary with repetitions. The collector the benchmark names
(`google-benchmark` by default) keeps one aggregate per benchmark (`compare.stat`, the median by
default). The table on stdout lists the gated metric of every benchmark, worst first; the tools'
own output goes to stderr.

A benchmark folder holds `manifest.json` (`target`, `links`, `repetitions`, `compare`), a
`CMakeLists.txt` that only calls `rfmap_perf_benchmark()` and the `*.cpp` sources. The target name
and the libraries come from the manifest; `RFMAP_PERF_CATALOG_DIR` points CMake at the catalog in
use, so `--catalog-dir` works for benchmarks too.

Each run gets a folder `tools/perf/results/<UTC time>/` (`--results-dir` to move it; it is not
committed):

| file | content |
|---|---|
| `environment.json` | git commit and dirty flag, CPU model, cores, memory, kernel, OS, cgroup limits, CPU governor, CMake build type, compiler and flags, and what Google Benchmark saw (CPU MHz, caches, load) |
| `events.json` | start and end of every phase (the run, the build, each benchmark); the reports draw them as bands on every chart |
| `results.json` | every measurement with its baseline, change and verdict |
| `raw/` | the binaries' JSON |

All runs also go into `results/history.sqlite`.

**Baseline and gate.** A measurement is compared with the median of its last 10 values
(`--window`) from earlier runs on the same host, with the same dataset and the same major version
of the benchmark and the collector. The host is a fingerprint of the CPU model, the number of
CPUs, the memory and the cgroup limits, so a recreated VM of the same type continues the history.
A change worse than `compare.maxRegressionPct` (5 % by default) is a regression and `run` exits 1;
`--no-gate` reports it but exits 0. With fewer than 3 earlier values there is no verdict yet.

Notes:

- Shared VMs are noisy: one repetition can take twice as long as the next. Compare on a quiet,
  dedicated machine with a fixed CPU governor, and keep `repetitions` at 10.
- Ubuntu's `libbenchmark` reports that it "was built as DEBUG". That concerns the library's own
  timing loop, not the code under test, which is built in Release.

## Terrain dataset (`dataset.py`)

Load tests must run against the same terrain every time, or results of different builds cannot
be compared. `dataset.py` builds that terrain once from real data and checks it before each run.

It uses the [tiles builder](../../docs/admin/tiles-builder.md) image to download Copernicus DEM
GLO-30 and ESA WorldCover for a region and to write the two rasters the radio calculations read:
`dem.cog.tif` and `landcover.cog.tif`. Hillshade, contours and the basemap are not built, because
the server does not read them. Next to the rasters it writes `perf-dataset.json` with the region,
its bounding box and the SHA-256 of each raster.

You need Docker and the tiles-builder image:

```bash
docker build -t rfmap/tiles-builder tools/tiles-builder
```

Build the default region, 25 km × 25 km around the centre of Warsaw:

```bash
python3 -I tools/perf/dataset.py build --out ../rfmap-perf-data
```

Keep the directory and reuse it between runs. Before a run, check that nothing changed:

```bash
python3 -I tools/perf/dataset.py verify --out ../rfmap-perf-data
```

Options of `build` and `bbox`:

| option | meaning |
|---|---|
| `--preset small\|medium\|large` | square region of 10, 25 (default) or 50 km |
| `--size-km 30` or `--size-km 30x15` | any size from 1 to 200 km, width × height, instead of a preset |
| `--center 52.23,21.01` | centre of the region as `LAT,LON` (default: central Warsaw) |
| `--image NAME` | tiles-builder image (`build` only, default `rfmap/tiles-builder`) |
| `--force` | rebuild even if tiles-builder sees no change (`build` only) |

`bbox` only prints the bounding box, for example to pass it to other tools.

Notes:

- The outputs land in `<out>/out`; downloads are cached in `<out>/cache`, so a second `build`
  downloads nothing.
- A region must not cross 50°, 60°, 70° or 80° of latitude, where Copernicus DEM changes its
  pixel spacing, nor the 180th meridian. The script stops before calling Docker in that case.
- At about 25–45 MB of DEM per 1° × 1° (see the tiles builder guide), the default region gives a
  few MB of rasters **(estimate)**.
- The data licences and attribution are the tiles builder's; see its guide.

Exit codes: 0 on success, 1 when the build or the check fails, 2 on a usage error.

Tests of both tools: `python3 -I -m unittest discover -s tools/perf -p '*_test.py'`.
