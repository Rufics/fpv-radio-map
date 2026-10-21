# Performance tools

Tooling for the load and performance tests of the C++ code (core, platform, FPV modules and the
server): the terrain dataset builder, the catalog of descriptions and extensions, the micro
suite (Google Benchmark), HTTP scenarios with k6 on a Docker stand with Prometheus and Grafana,
sanitizer runs, the history with its baseline, HTML and Markdown reports, and flamegraphs of a
regression. CI runs only the unit tests of
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
| `dashboard` | `extensions/dashboards/<id>/` | extension | a Grafana dashboard for the stand's Grafana |

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
(`--window`) from earlier runs on the same host, with the same dataset, the same collectors and
the same major version of the benchmark and the collector. The host is a fingerprint of the CPU model, the number of
CPUs, the memory and the cgroup limits, so a recreated VM of the same type continues the history.
A change worse than `compare.maxRegressionPct` (5 % by default) is a regression and `run` exits 1;
`--no-gate` reports it but exits 0. With fewer than 3 earlier values there is no verdict yet.
`--repetitions` must be 2 or more, since Google Benchmark writes the median only then.

Notes:

- Shared VMs are noisy: one repetition can take twice as long as the next. Compare on a quiet,
  dedicated machine with a fixed CPU governor, and keep `repetitions` at 10.
- Ubuntu's `libbenchmark` reports that it "was built as DEBUG". That concerns the library's own
  timing loop, not the code under test, which is built in Release.

## HTTP scenarios (`perf.py run --scenario ID`)

A scenario puts load on the real server through its HTTP API. The stand is a Docker Compose project
`rfmap-perf` from `stand/compose.yml`: PostGIS, the server built from `deploy/` with
`stand/server.perf.json`, and k6 (`grafana/k6:1.0.0`, pinned by digest) running `k6/runner.js`.

```bash
python3 -I tools/perf/dataset.py build --out ../rfmap-perf-data     # once, see "Terrain dataset"
python3 -I tools/perf/perf.py run --scenario smoke --dataset ../rfmap-perf-data
```

| scenario | load |
|---|---|
| `smoke` | 4 requests per second for 60 s; a quick check that every persona works |
| `peak` | warm-up at 5 rps, ramp to 30 rps, 5 minutes at 30 rps, ramp down |
| `auth-storm` | link opens and PIN guesses ramp to 20 rps for two minutes next to normal viewers: argon2id takes the CPU, the viewers' latency shows the cost |
| `soak` | an hour at 8 rps; fails when the server's memory or open files keep growing in the second half (`limits`) |

A scenario mixes **personas** by weight. A persona is what one kind of user does: its auth
(`link-pin`, `link-token`, `mission-owner` or none), weighted steps with method, path, body and the
expected status or statuses,
and an optional `setup` that creates missions from a fixture before the load starts.

| persona | does |
|---|---|
| `link-editor` | edits a mission through its link: calc-path, calc-route, reads the mission |
| `viewer` | opens a shared link: reads the mission, lists radios, health |
| `coverage-planner` | starts a coverage job as the mission owner and polls it until it is done |
| `mission-creator` | creates missions without an account |
| `link-opener` | opens a shared link with the right PIN on every request (`link-token`: the step opens the link itself) |
| `pin-guesser` | sends wrong PINs to shared links until they are paused, then keeps knocking (expects 401 or 429) |

Fixtures hold request bodies made from the Warsaw dataset with a fixed seed: `warsaw-paths`
(400 calc-path bodies) and `warsaw-missions` (40 missions with a coverage grid). A step whose
body is an array file sends its elements in turn.

What `run --scenario` does:

1. checks the dataset against its manifest (`--dataset`, the `--out` of `dataset.py`);
2. builds the server image (`--no-build` uses `--server-image` as it is) and starts the stand with
   a fresh database on `127.0.0.1:18080` (`--port`), waiting for `/health`;
3. creates the personas' missions and links, then writes `plan.json`: one k6 scenario per persona
   with its share of the arrival rate, the bodies and the credentials;
4. runs k6; `k6-summary.json` gives p50, p95, p99, rps and the error rate of the whole run and of
   each `persona/step`, and the p95 of coverage jobs;
5. saves the server log to `raw/rfmap-server.log` and stops the stand (`--keep-stand` leaves it
   running).

The run folder and the baseline work as in the micro suite. `events.json` also has the stand's
phases and every load stage (warm-up, ramp, plateau), placed by the time k6 started. The dataset
key is `terrain-<hash of the dataset manifest>` and the personas and fixtures count as involved
items, so a new major version of either starts a new baseline. `environment.json` also has the
server's settings, the PostgreSQL version, settings and database size, and the containers'
limits.

**Monitoring.** Unless `--no-monitoring` is given, the stand also runs Prometheus
(`127.0.0.1:19090`, `--prometheus-port`), cAdvisor and postgres_exporter. Prometheus scrapes every
5 s the server's `/api/v1/admin/metrics` (with the run's admin token), the containers and
PostgreSQL, and k6 writes its own series in with remote write. After the load, the `prometheus`
collector reads its queries over the load phase and gives one measurement per statistic:

| query | from |
|---|---|
| `server_cpu`, `server_rss`, `server_threads`, `server_fds` | the server's own `rfmap_process_*` |
| `server_rps`, `server_5xx`, `server_p95` (by rate class) | `rfmap_http_*` |
| `pool_queued`, `pool_busy` (by pool), `db_pool_waiting`, `db_pool_wait` | the thread and database pools |
| `jobs`, `cache_hit_ratio`, `cache_used` (by tier) | jobs and caches |
| `db_cpu`, `db_memory`, `k6_cpu`, `container_cpu`, `container_memory` | cAdvisor |
| `pg_connections`, `pg_transactions`, `pg_size` | postgres_exporter |
| `k6_p95` (by persona), `k6_vus` | k6 |

A query with several series gives one measurement per series (`pool_queued/http`). Every series,
from a minute before the load to a little after it, is saved to `raw/prometheus.json` for the
reports. The queries live in the collector's manifest (`options.queries`: name, PromQL, unit,
better, stats out of `max`, `min`, `avg`, `last` and `slope` per hour), so a new one needs no code.
These measurements are recorded and compared but only gate the run when the scenario names them:
`compare.metrics` takes a metric of every measurement (`p95`) or one measurement
(`server_rss:slope`). Runs with and without monitoring are never compared with each other.

With `--keep-stand`, Grafana runs too (`http://127.0.0.1:13000`, `--grafana-port`, no login) with
the `dashboard` extensions of the catalog (`extensions/dashboards/<id>/`: a manifest and a Grafana
JSON model) and the run's phases and load stages as annotations.

cAdvisor finds the containers by their cgroup parents (`rfmapperf_<service>.slice`): its Docker
integration does not see containers on the containerd image store, the default since Docker 29.
Where a sandbox gives no memory cgroup for a custom parent, the cAdvisor memory series stay empty;
the server's own memory comes from the server and is unaffected.

**Limits.** Next to the baseline, a scenario can set absolute `limits` by `METRIC` or
`NAME:METRIC`, and a collector can set `failAbove` on its metrics. A measurement above one fails
the run whatever the history says; the problems lead `results.json` and both reports. `soak` uses
`server_rss:tail_slope` and `server_fds:tail_slope`, the growth per hour over the second half of
the load, after the caches filled.

**Sanitizers.** `--sanitize` runs the scenario against the server built with AddressSanitizer,
LeakSanitizer and UndefinedBehaviorSanitizer (`stand/server-sanitize.Dockerfile`, image
`rfmap-server:perf-sanitize`, added by `stand/compose.sanitize.yml`). After the load perf.py stops
the server gracefully, so LeakSanitizer reports what was still allocated, and the `sanitizer`
collector counts the reports in `raw/sanitizer/`: any error, leaked byte or undefined behaviour
fails the run. Sanitized runs are 2-3 times slower and are only compared with each other.

Limits of the stand's containers come from the caller's environment:

| variable | default |
|---|---|
| `RFMAP_PERF_SERVER_CPUS`, `RFMAP_PERF_SERVER_MEMORY` | `4`, `4g` |
| `RFMAP_PERF_DB_CPUS`, `RFMAP_PERF_DB_MEMORY` | `2`, `2g` |
| `RFMAP_PERF_K6_CPUS` | `2` |

Prometheus gets 1 CPU, cAdvisor half and postgres_exporter a quarter; on a small machine that
is load of its own, which is one more reason to keep a baseline per machine.

Notes:

- The stand's server config has metrics on, no weather, very high rate limits and the production
  password hashing cost, so the load measures the server and not its limiter.
- Behind a proxy, Docker needs the proxy for pulls and builds; with Docker Hub rate limits, a
  registry mirror in `/etc/docker/daemon.json` helps.

## Reports

At the end of every `run`, each `reporter` extension writes its file into the run folder
(`--no-report` skips them; a scenario's `reporters` picks some instead of all):

| reporter | writes |
|---|---|
| `html` | `report.html`: the verdict, the environment (machine, build, server and PostgreSQL settings), the timeline of phases, a chart per Prometheus query with the phases and load stages shaded, the trend of every gated measurement over the last 30 runs on the same host and dataset (baseline, allowed change and a marker where an item changed its version), and every measurement |
| `markdown` | `summary.md`: the verdict and the gated measurements in GitHub Markdown, for a pull request |

```bash
python3 -I tools/perf/perf.py report tools/perf/results/<run> [--reporter html]
```

writes them again, for example for older runs after a reporter changed. `report.html` is one
static file; Plotly comes from jsDelivr, pinned with its integrity hash (option `plotly` of the
`html` reporter points elsewhere). A reporter is a class derived from `perfkit.report.Reporter`
whose `render()` gets the run's files and the trends.

## Flamegraphs of a regression

When a run has a gated regression, every `profiler` extension profiles it before the results are
written (`--no-profile` skips this), and both reports show the result:

- **micro:** each regressed benchmark (the five worst at most) runs once more alone, for 5 s,
  under the profiler (`--benchmark_filter=^NAME$`);
- **scenario:** the stand comes up again and gets the scenario's highest rate as a constant load;
  after `profile.warmUpS` (10 s) the profiler attaches to the server for `profile.durationS`
  (30 s). The set-up and k6 of this second load go into `profile/` of the run folder.

The `flamegraph` profiler samples the call stacks with Linux perf (`frequencyHz` 99, DWARF
unwinding, so the release build needs no frame pointers) and draws `raw/flamegraph-NAME.svg` with
[inferno](https://github.com/jonhoo/inferno); it keeps the folded stacks `raw/NAME.folded`, so
two runs can be compared with `inferno-diff-folded`. It needs `perf` (`linux-perf` or
`linux-tools`) and `cargo install inferno`; attaching to the server needs root or
`kernel.perf_event_paranoid` ≤ 1. `RFMAP_PERF_PERF` names another perf binary, for a kernel
without a matching perf package. A profile that fails is noted in `results.json` (`profiles`) and
in the reports; it does not fail the run. A profiler is a class derived from
`perfkit.profile.Profiler`.

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
