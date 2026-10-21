# Performance tools

Tooling for the load and performance tests of the C++ code (core, platform, FPV modules and the
server). Only the terrain dataset builder exists so far; the orchestrator, benchmarks and k6
scenarios follow.

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

Tests: `python3 -I -m unittest discover -s tools/perf -p '*_test.py'`.
