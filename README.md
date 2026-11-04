<div align="center">

# FPV Radio Map

**Will my control and video link reach the target, and if not, why?**

Plan an FPV route on the map and see each radio link the way the terrain sees it:
Fresnel zone, ridge diffraction, coverage heatmap, maximum range, battery, and a fix you can preview before applying it.

[![Main](https://github.com/Rufics/fpv-radio-map/actions/workflows/main.yml/badge.svg?branch=main)](https://github.com/Rufics/fpv-radio-map/actions/workflows/main.yml)
[![CI](https://github.com/Rufics/fpv-radio-map/actions/workflows/ci.yml/badge.svg)](https://github.com/Rufics/fpv-radio-map/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Rufics/fpv-radio-map?sort=semver&display_name=tag&label=release)](https://github.com/Rufics/fpv-radio-map/releases)
[![License: MIT](https://img.shields.io/github/license/Rufics/fpv-radio-map)](LICENSE)
<br>
[![Status: alpha](https://img.shields.io/badge/status-alpha-orange)](#roadmap)
[![Conventional Commits](https://img.shields.io/badge/Conventional%20Commits-1.0.0-fe5196?logo=conventionalcommits&logoColor=white)](https://www.conventionalcommits.org/en/v1.0.0/)
[![semantic-release](https://img.shields.io/badge/release-semantic--release-e10079?logo=semantic-release)](https://github.com/semantic-release/semantic-release)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](core/rf-core)
[![Angular](https://img.shields.io/badge/Angular-22-DD0031?logo=angular)](web)
[![MapLibre](https://img.shields.io/badge/MapLibre-GL-396CB2?logo=maplibre)](web)

<img src="docs/images/laptop-result.png" alt="Route result on a laptop: control link reaches the target, video link is blocked by a ridge on leg 3, battery is 5.9 km short" width="100%">

<sub>Screenshots are from the demo build with sample data (<code>npm run build:mock</code> in <code>web/</code>).</sub>

</div>

## Why

FPV pilots lose links behind hills, past ridges and at the far end of long routes, usually without knowing which of these
it was. Generic coverage maps answer "is there signal here". This project answers the questions a pilot asks before take-off:

- **Will the control link and the video link hold along the whole route?** Each is evaluated separately: different
  frequency, power, antennas and receiver sensitivity.
- **If a point is unreachable, what blocks it?** The terrain profile, the first Fresnel zone, the obstructing ridge and
  its diffraction loss, in dB.
- **What would fix it, and what does the fix cost?** Fly higher on one leg, move the launch point, add a relay, turn
  back earlier. Every fix is previewed before it changes the mission.
- **How sure is the answer?** The stated error of the elevation data is carried through the calculation. A verdict
  reads *reliable*, *marginal* or *blocked*, and says when the result could go either way.

### Principles

- **Exact maths, no fudge factors.** Fresnel integrals and the knife-edge function J(ν) are computed exactly,
  diffraction uses the Bullington method on a k·R earth, and distances use Karney's geodesics on WGS-84. Where an input
  has error we don't control (DEM, forecast, datasheets), the result says so instead of hiding it.
- **Reference data lives in the database, not in code.** Radio profiles, antennas, batteries and drones come from a
  library that only the server admin can mark as verified. Every seeded value names its source.
- **Extensible and safe.** Features ship as signed modules (ed25519). Modules from third parties run in a separate,
  sandboxed process, and only the admin installs them.

## What it looks like

<table>
<tr>
<td width="50%"><img src="docs/images/laptop-why.png" alt="Why leg 3 fails: terrain profile with the Fresnel zone cut by a 622 m ridge, full link budget"></td>
<td width="50%"><img src="docs/images/laptop-fix-preview.png" alt="Fix preview: fly leg 3 at 345 m AGL, before and after table"></td>
</tr>
<tr>
<td><b>Why it fails.</b> Terrain profile with the first Fresnel zone, the obstructing ridge and every term of the link budget.</td>
<td><b>Preview a fix.</b> Nothing changes until you apply it. The table shows the effect on every link and the battery.</td>
</tr>
<tr>
<td><img src="docs/images/laptop-compare.png" alt="Scenario compare: worst margins of A and B per link, route coloured by the difference"></td>
<td><img src="docs/images/laptop-share.png" alt="Share dialog: read-only links protected by a PIN, KML and GPX export"></td>
</tr>
<tr>
<td><b>Compare two setups.</b> For example a different antenna or frequency: worst margins side by side, and the route coloured by B − A.</td>
<td><b>Share and export.</b> Read-only links protected by a PIN that the owner can revoke, plus KML and GPX.</td>
</tr>
<tr>
<td><img src="docs/images/laptop-fixed-wing.png" alt="Fixed-wing drone: flight checks card with the stall margin in a 45 degree turn"></td>
<td><img src="docs/images/laptop-autonomous.png" alt="Autonomous route: lost video link on legs 3 and 4 shown as warnings, with the links lost on the way card"></td>
</tr>
<tr>
<td><b>Plan a fixed-wing.</b> The route is checked against the plane's envelope: climb and dive angles over the terrain, turn radius at each waypoint and the stall margin in a banked turn.</td>
<td><b>Fly an uploaded route.</b> In autonomous mode the links are optional. A lost link is a warning that shows where the pilot cannot take over or loses the picture, not a block.</td>
</tr>
<tr>
<td><img src="docs/images/laptop-3d-altitude.png" alt="3D view of the route over the terrain, with the altitude profile, the safe corridor and the climb assistant's strategy cards"></td>
<td><img src="docs/images/laptop-weather.png" alt="Weather mode: the route coloured by head and tail wind, the forecast at every point at its arrival time, and a read-out on leg 3"></td>
</tr>
<tr>
<td><b>See the route in 3D.</b> The altitude profile under the map shows the ground, the plan and the safe corridor; the climb assistant compares the plan with the energy-optimal climb, and <b>▶ Chase</b> flies the plan.</td>
<td><b>Weather on the route.</b> The forecast at each point at the time the drone gets there: wind against the route, temperature and rain, with a pre-flight briefing.</td>
</tr>
<tr>
<td><img src="docs/images/laptop-replay.png" alt="Flight replay of a Betaflight Blackbox log: the flight's story, the track over the plan coloured by cell voltage and the timeline with flight modes"></td>
<td><img src="docs/images/laptop-offline-draw.png" alt="Offline: drawing an area on the map to have the server build a package for it"></td>
</tr>
<tr>
<td><b>Replay a flight.</b> Open a Betaflight Blackbox log over the mission: the track, the flight's story, battery, altitude, speed and link on a timeline, a logbook, and plan against fact.</td>
<td><b>Draw your own offline area.</b> Click the corners of the area you need; the server builds its package, and the browser keeps it for use without a network.</td>
</tr>
<tr>
<td><img src="docs/images/laptop-offline-regions.png" alt="Offline regions: three region packages with preview, area, size, layers and version, each with a Download button"></td>
<td><img src="docs/images/admin-offline-regions.png" alt="Admin data sources: offline regions with their state, ready or building, and a Rescan regions button"></td>
</tr>
<tr>
<td><b>Take a region offline.</b> The server cuts each region out of one world map set and packs the map, relief, 3D terrain, style and fonts into one zip with a version.</td>
<td><b>See the packages build.</b> The admin page shows each region's state and last error, and rescans the region list on demand.</td>
</tr>
</table>

<p align="center">
<img src="docs/images/phone-mission.png" alt="Phone: shared mission, view only" width="24%">
&nbsp;
<img src="docs/images/phone-why.png" alt="Phone: why the leg fails" width="24%">
&nbsp;
<img src="docs/images/admin-modules.png" alt="Admin: installed modules with lifecycle and signature" width="48%">
<br>
<sub>Phone layout for the field, and the admin page for signed modules.</sub>
</p>

## Features

| | Feature | Where |
|---|---|---|
| 📡 | Control and video links, each with its own radio, antennas and sensitivity; evaluated separately or together | server, `rf-core` |
| ⛰️ | Terrain profile, first Fresnel zone, Bullington diffraction, coverage heatmap and maximum range | `rf-core` |
| 🗺️ | Coverage that keeps the map readable: under the relief and labels, fading while you edit, an optional mini-map, and line of sight to the cursor | web |
| 🌦️ | Refraction from the forecast (ITU-R P.453) and rain loss (ITU-R P.838-3), including future dates | `rf-ext`, Open-Meteo |
| 🌬️ | Weather along the route at each point's arrival time: head and tail wind per leg, temperature and rain, a weather map and a pre-flight briefing | server, web, Open-Meteo |
| 🌲 | Forest and buildings from land-cover classes, with obstacle heights set by the admin | `rf-ext` |
| ± | Uncertainty band from the DEM's stated error: *reliable*, *marginal*, *blocked*, *blocked, may work* | `rf-ext` |
| 🔋 | Battery range with wind, temperature and climb, and a turn-back point when the pack won't last | `fpv.battery` |
| ↕️ | Minimum altitude along the route that keeps the Fresnel zone clear | `fpv.min-altitude` |
| 🏔️ | 3D view of the route over the terrain, with a linked altitude profile, the safe corridor (terrain clearance, link floor, ceiling) and an optional Fresnel tube | `fpv.altitude`, web |
| 📈 | Waypoint heights planned above the launch point (or above ground), filled in automatically from the corridor floor, and a climb assistant that compares the plan with the energy-optimal climb | `fpv.altitude` |
| 🎥 | Chase camera that flies the plan in 3D, and an altitude sheet with the worst spot of the plan on the phone | web |
| 🏠 | Failsafe check: where the link is lost, and whether return-to-home clears the terrain | `fpv.failsafe` |
| ✈️ | Fixed-wing planning: the climb angle over the terrain from take-off on, the turn radius at every waypoint, the stall margin in a banked turn, and loiter instead of hover in the failsafe | `fpv.airframe`, `fpv.failsafe` |
| 🛰️ | Autonomous routes uploaded before take-off: the radio links become optional, and a lost link shows where the pilot cannot take over or loses the picture instead of blocking the route | server, `fpv.failsafe` |
| 📍 | Best launch point inside an area you draw | `fpv.best-launch` |
| 🔁 | Relay placement: budget per hop and the best relay spot | `fpv.relay` |
| ⚖️ | Scenario compare, KML and GPX export | `fpv.core` |
| 🎞️ | Flight replay from Betaflight Blackbox logs: track, battery, altitude, speed, link, modes and failsafe on the map, the flight's story, a logbook, and plan against fact | `fpv.blackbox`, `rf-flightlog` |
| 🔗 | Read-only share links with a PIN; only the owner key revokes them | server |
| 📦 | Offline regions: packages of the map, relief and 3D terrain cut from one world PMTiles set by a box, an admin boundary or a polygon, or built for an area a user draws; saved in the browser for use without a network, with an *update available* flag when the server has a newer version | server, web |
| 📚 | Equipment library: radios, antennas, batteries, drones, with sources and admin-verified flags | server, web |
| 🩺 | Operations: liveness, readiness and Prometheus metrics on a port of their own, a graceful drain on stop, bounded calculations and database waits, and admin settings kept in step across server processes | server |

## Architecture

```mermaid
flowchart LR
  subgraph Browser
    W["web/ · Angular + MapLibre<br/>layout A (laptop) · layout C (phone)"]
  end
  subgraph Server["rfmap-server (C++20)"]
    API["HTTP API<br/>docs/api/openapi.yaml"]
    CORE["rf-core<br/>geodesy · Fresnel · diffraction · budget"]
    EXT["rf-ext<br/>refraction · rain · clutter · uncertainty"]
    REG["rfmod registry<br/>signature · schema · deps · self-test"]
    POOL["rf-db<br/>bounded session pool"]
    SYNC["settings sync<br/>LISTEN/NOTIFY + poll"]
    PKG["offline packages<br/>go-pmtiles extract"]
    OPS["ops listener :9090<br/>/healthz · /readyz · /metrics"]
  end
  subgraph Modules["Signed modules (.rfmod)"]
    OWN["owner-signed<br/>in process"]
    THIRD["third-party<br/>isolated worker"]
  end
  DB[("PostgreSQL + PostGIS<br/>missions · library · flight logs<br/>settings · cache")]
  DEM[["DEM (COG) · land cover<br/>PMTiles basemap"]]
  OM["Open-Meteo<br/>(self-hosted)"]

  W -->|/api/v1| API
  W -->|/tiles| DEM
  API --> CORE --> EXT
  API --> REG --> OWN
  REG --> THIRD
  API --> POOL --> DB
  SYNC --> DB
  API --> PKG --> DEM
  CORE --> DEM
  EXT --> OM
```

| Path | What it is |
|---|---|
| [`core/rf-core`](core/rf-core) | Units, WGS-84 geodesy, exact Fresnel integrals, Bullington diffraction, link budget |
| [`core/rf-ext`](core/rf-ext) | Refraction, rain, clutter, uncertainty analysis |
| [`core/rf-flightlog`](core/rf-flightlog) | Normalized flight logs into the replay timeline, the flight's story and plan against fact |
| [`core/rf-db`](core/rf-db) | The database layer: a bounded, health-checked connection pool behind abstract connection and factory interfaces; [`pg/`](core/rf-db/pg) implements it for PostgreSQL (libpqxx) with connect, statement, lock and idle-in-transaction timeouts |
| [`platform/rfmod`](platform/rfmod) | Module ABI, signed package format, registry, sandboxed worker, `rfmod` CLI |
| [`modules/fpv`](modules/fpv) | The FPV module pack |
| [`server`](server) | HTTP API, PostGIS storage, raster access, weather, result cache |
| [`cli`](cli) | `rfmap`, the command-line client of the API: calculations and data as JSON for scripts, CI and other services |
| [`web`](web) | `@rfmap/ui-kit` and the FPV app |
| [`tools/tiles-builder`](tools/tiles-builder) | Builds the DEM COG, hillshade, contours, basemap and satellite PMTiles |
| [`tools/perf`](tools/perf) | Performance and load test tooling: the fixed terrain dataset for load tests |
| [`tools/ci`](tools/ci), [`tools/wiki`](tools/wiki), [`tools/blackbox`](tools/blackbox), [`tools/app-icons`](tools/app-icons) | Repository tooling, all in Python: CI reports, coverage and the Sonar scope, the wiki publisher, the Betaflight mode tables and the app icons |
| [`schemas`](schemas) | JSON Schemas of the mission, the flight log, the library kinds and the module manifest, shared by the server and the browser |
| [`fuzz`](fuzz) | Fuzz targets for the C++ input parsers |
| [`deploy`](deploy) | Docker Compose: PostGIS, Open-Meteo, server, web |
| [`examples/modules/body-shadow`](examples/modules/body-shadow) | A small example module to start from |

## Quick start

**Try the interface with sample data** (no server needed):

```sh
cd web
npm ci --ignore-scripts
npm run start:mock-prod      # then open http://localhost:4200
```

**Run the whole stack** with Docker. You need terrain tiles from the [tiles builder](docs/admin/tiles-builder.md) first.
[Production server, step by step](docs/admin/production-setup.md) walks through tiles, keys, modules, TLS and backups.

```sh
cd deploy
cp .env.example .env         # set POSTGRES_PASSWORD, RFMAP_ADMIN_TOKEN, RFMAP_TILES_DIR
docker compose up -d --build
```

Or use the published images: `ghcr.io/rufics/fpv-radio-map-server` and `ghcr.io/rufics/fpv-radio-map-web`
(`:edge` follows `main`; releases are tagged `X.Y.Z`). See [docs/admin/deploy.md](docs/admin/deploy.md).

**Build and test from source** (Linux; Ubuntu 26.04 packages are listed in [CONTRIBUTING.md](CONTRIBUTING.md#checks)):

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

## Documentation

All documentation lives in [`docs/`](docs/README.md) and is published to the [wiki](https://github.com/Rufics/fpv-radio-map/wiki) on every change.

- [Glossary](docs/guide/glossary.md) and [FAQ](docs/guide/faq.md)
- [Command-line client](docs/guide/cli.md): calculations and data from the server for scripts and CI
- [Production server, step by step](docs/admin/production-setup.md), then [server deployment](docs/admin/deploy.md) and the [tiles builder](docs/admin/tiles-builder.md) for every option
- [Offline regions](docs/admin/offline-regions.md), [how map layers reach the browser](docs/admin/map-layers.md) and [how the weather forecast reaches the pilot](docs/admin/weather-data.md)
- [HTTP API (OpenAPI)](docs/api/openapi.yaml)
- [Writing a module](docs/modules/developer-guide.md): C ABI, host calls, UI bundles, signing, deprecation
- [Web frontend](web/README.md)
- [Database migrations](docs/dev/migrations.md), [fuzz testing](docs/dev/fuzzing.md) and [performance tests](tools/perf/README.md)
- [Contributing](CONTRIBUTING.md): commit standard, checks, releases

## Roadmap

The core is complete and tested. The project is **alpha**: the interfaces may still change before 1.0.

**Next**
- [ ] Relay planning in the interface (the `fpv.relay` module already computes it)
- [ ] Route editing: snap to terrain features, and undo for every edit (today only the last point move can be undone)
- [ ] Battery summary in the route result
- [ ] Validate weather against a live self-hosted Open-Meteo instance
- [ ] Time limits for owner-signed modules that run in the server process
- [ ] A coverage request on a given grid, for exact difference maps between any two scenarios
- [ ] A lighter export call that does not recompute the route

**Later**
- [ ] Real antenna patterns: patch, helical, dipole, pointing and bank angle, polarization mismatch
- [ ] Interference zones with a raised noise floor
- [ ] Choice of elevation data: Copernicus 30 m, SRTM or your own LiDAR
- [ ] PDF mission report
- [ ] Verified radio profiles for more systems, each with its published source

Have an idea or need? [Open an issue](https://github.com/Rufics/fpv-radio-map/issues/new/choose).

## Contributing

Contributions are welcome: code, verified equipment data with sources, modules, documentation and bug reports.
Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). Report security issues privately,
as described in [SECURITY.md](SECURITY.md).

## License and data

The code is released under the [MIT License](LICENSE).

The map data has its own terms, and each deployment must show their credits. The web app shows them from the tiles
builder's `manifest.json`, and the exact texts are in [the tiles builder guide](docs/admin/tiles-builder.md#8-attribution-to-show-on-the-map):
- **Elevation:** Copernicus DEM GLO-30, © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018, provided
  under COPERNICUS by the European Union and ESA
- **Basemap:** © OpenStreetMap contributors (ODbL 1.0), © OpenMapTiles (CC BY 4.0)
- **Land cover:** © ESA WorldCover project 2021 (CC BY 4.0)
- **Satellite imagery:** whichever mosaic the operator builds, with the licence and credit they pass to the tiles
  builder. Licences differ by provider and year, and some allow only non-commercial use.

Check each provider's current terms before a public deployment.
