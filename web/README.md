# FPV Radio Map — web frontend

Angular 22 (standalone components, signals, zoneless), MapLibre GL 5 with PMTiles, strict TypeScript. The UI is English, metric and dark theme only.

```
web/
  projects/ui-kit/    @rfmap/ui-kit — reusable library (map, layers, slots, state, charts, formatting)
  projects/fpv-app/   the FPV app, built only from ui-kit plus FPV panels
  scripts/            sync-schemas, serve-dist, screenshots
  screenshots/        Playwright screenshots of the mock build
  API-NOTES.md        contract gaps and the assumptions made for them
```

## Requirements

Node ≥ 22.22.3 or ≥ 24.15 (Angular CLI 22). CI uses Node 24.

```sh
npm ci --ignore-scripts
```

`npm run sync-schemas` runs before start, build and test. It copies `../schemas/mission.schema.json` and `../schemas/library/*.schema.json` into `projects/ui-kit/src/lib/schemas/schemas.generated.ts`, so the browser validates with the same JSON Schemas (draft-07, ajv strict) as the server.

## Run, build, test

| Command | What it does |
|---------|--------------|
| `npm start` | `ng serve fpv-app`, development configuration, **dev mock on**. Calls go to `/api/v1`; with the mock off, proxy that path to the server. |
| `npm run build` | `ng build ui-kit && ng build fpv-app`. Production uses the real API and contains no mock code. |
| `npm run build:mock` | Production build with the dev mock, into `dist/fpv-app-mock`. |
| `npm test` | Vitest unit tests for ui-kit and fpv-app. |
| `npm run screenshots` | Serves `dist/fpv-app-mock/browser` and writes Playwright screenshots to `screenshots/`. Run `npm run build:mock` first. It uses the Chromium in `PLAYWRIGHT_BROWSERS_PATH` (default `/opt/pw-browsers`); there is no `playwright install`. |

## Dev mock

The flag is `environment.mockApi`. It is `true` in `environment.development.ts` and `environment.mock.ts`, and `false` in `environment.ts`. Those two configurations also swap in `app/mock-entry.mock.ts`, which lazy-loads `app/mock/*`:

- An `HttpInterceptor` answers every `/api/v1` call from an in-memory backend (`MockBackend`) with realistic latency. Jobs finish part by part over about 3 s.
- `/weather/point` answers for the place asked: the wind freshens and veers towards the north-east, it cools with height and rain sets in east of the ridge, so the weather along the route changes. Without a place it is the reference value at the pilot.
- `mockdem://`, `mocksat://` and `mockwx://` map protocols draw terrain, satellite and weather tiles locally, and the mock map config adds a GeoJSON base map and contours. No third-party map service is used.
- It serves the reference case near Dnipro (48.53 N 35.20 E). Leg 3 WP2 → B fails on video 915 MHz: FSPL 106.13 dB, ridge 622 m diffraction 29.61 dB, received −98.74 dBm, required −95.00 dBm, margin −3.74 dB. Control 433 MHz holds at +6.01 dB. The fix "leg 3 at 345 m AGL" gives +25.32 dB. The battery has 10.2 km of range against 16.1 km needed, and turns back at T after 6.0 km.

Demo data:

| What | Value |
|------|-------|
| Seeded mission | `/mission/m-north-ridge`, owner key `owner-demo-key` |
| View link | `/m/v/demo-view`, PIN `246810` |
| Edit link | `/m/e/demo-edit`, PIN `135790` |
| Wrong PIN | 5 tries, then the link pauses for 15 min (429 with `retryAfterS` + `Retry-After`) |
| Expired link | `/m/v/demo-expired` (410) |
| Revoked link | `/m/v/demo-revoked` (410) |
| Admin token | any non-empty token |
| Scenarios | `?scenario=missing-elevation`, `module-failed`, `stale-forecast`, `normal` (kept for the tab) |

## ui-kit structure (`@rfmap/ui-kit`)

| Folder | Contents |
|--------|----------|
| `format/` | `RfFormat` and the one shared `rfNum` pipe / `<rf-num>`. dB and dBm get 2 decimals; km gets 3 in breakdowns and 1 in summaries; heights are whole metres; the minus sign is U+2212; the full value goes in `title` and in exports. |
| `map/` | `<rf-map-view>`, `MapContext`, `provideMapView()`, the PMTiles protocol and the attribution line. |
| `layers/` | `LayerRegistry` (one per map, groups base → terrain → weather → results → mission), the standard layers (dark base on the OpenMapTiles schema, satellite, hillshade, contours, weather with a time slider) and `<rf-layer-panel>`. |
| `slots/` | `SlotRegistry` and `<rf-slot-host>` for `header.actions`, `side.panels`, `map.layers`, `map.overlays`, `bottom.drawer` and `verdict.reasons`. |
| `coverage/` | Heatmap with the 4-colour scale (`CoverageLayer`), radio and battery limits, and scenario difference: `subtractGrids`, `CoverageDeltaLayer`, `RouteDeltaLayer` and `DeltaScale` (▲/▼ marks as well as colour). |
| `charts/` | SVG terrain profile with the Fresnel zone and earth bulge, and the link-budget waterfall. |
| `tools/` | Geo helpers and `RouteTool` (markers, legs, drag, add point). |
| `weather/` | Weather along the route: `RouteTimeline` (arrival time at each place at the cruise speed), `RouteWeather` (forecast samples read at any distance, head/tail wind per leg), `WeatherScale` (classes and colours), `RouteWeatherLayer` (the route recoloured by the weather in weather mode) and `PolylineHitTest` (hover read-outs). |
| `state/` | `RfmapApiClient` (one method per contract operation), `MissionStore` (signals; save, calculate, job parts, preview fix with JSON merge patch), `CredentialStore` and `applyMergePatch`. |
| `modules/` | `ModuleUiLoader`: runtime UI bundles (see below). |
| `export/` | `MissionExporter`, a client-side KML/GPX fallback that writes AGL heights. |
| `schemas/` | `SchemaValidator` (ajv draft-07 strict plus formats). |
| `ui/` | Pill, segmented control, stepper, job progress and heat legend. |
| `styles/rfmap-ui-kit.css` | Design tokens (`--rf-*`), IBM Plex Sans/Mono with tabular figures, radii 6/8/12/16, and touch targets of 44 px (32 px for dense desktop). |

## How a module UI bundle registers into slots

A module can ship a UI bundle declared in its manifest, which the server reports as `ModuleInfo.ui = { bundle, slots }`. After loading `/modules`, the app's `ModuleUiLoader` imports each bundle with a dynamic import. It skips a bundle when:

- the module is not `active`, or is `alpha`;
- the bundle's origin is not the app's own or listed in `MODULE_BUNDLE_ORIGINS`;
- the bundle's `kit` major differs from the ui-kit major;
- it registers a slot that is not in `ui.slots`.

A failing bundle only marks itself failed, and `/admin/modules` shows the status.

```js
// public/modules/fpv-example/ui.js — a plain ES module, no framework
export const kit = '1';
export function register(api) {
  api.slots.register({
    id: 'flight-heights',
    slot: 'side.panels',            // must be listed in the manifest's ui.slots
    title: 'Flight heights',
    mount(host, ctx) {              // render into host; return a cleanup function
      return api.state.subscribe(({ mission, result }) => {
        host.textContent = mission ? `Highest ${api.format.format(Math.max(...mission.waypoints.map(w => w.aglM)), 'm')} AGL` : '';
      });
    },
  });
}
```

`api` contains `kitVersion`, `moduleId`, `moduleVersion`, `slots.register` (scoped to the module), `state` (read-only `mission()`, `result()`, `subscribe`) and `format` (the same `RfFormat` as the app). The example bundle adds the "Flight heights" panel and a note in `verdict.reasons`.

## FPV features on the fpv.core module

`fpv.core` is used only when `/modules` lists it as active. Its options go to `mission.modules["fpv.core"]`, and its output is read from `RouteResult.modules["fpv.core"].data`.

- **Compare (feature 13):** `options.compare = { name, patch }`, where `patch` is a JSON merge patch on the mission. The compare panel offers presets (video or control radio, video power, ground antenna, airframe) or a custom patch. It shows A and B worst margins side by side, with B − A and the counts of better and worse points. When the route is aligned, the route gets a delta-coloured casing. "Coverage difference map" runs a second coverage job for B and subtracts the grids client-side, cell by cell, only where the cells coincide exactly. Grids are never resampled; cells without an exact partner are hatched grey and labelled "not comparable".
- **Export (feature 14):** the Share dialog asks for `options.export = [kind]` with `kmlAltitude` (above ground or above sea level) through `/calc/route`, then downloads `data.export[kind].content` as a Blob. Without fpv.core, the browser writes KML/GPX itself, with AGL heights only.
- **Equipment library (feature 16):** `/admin/library` over `/library/{kind}`. Everyone can browse it read-only. Only the admin token allows edits; entries are validated live with ajv and on the server. Verified and unverified status and the source are shown in the library list and editor, in the radio setup dialog, and in the compare pickers.

## Routes

| Route | Page |
|-------|------|
| `/` | Workspace: layout A on laptop/desktop, layout C on phones (≤ 767 px) |
| `/mission/:id` | Workspace for a saved mission |
| `/m/:role/:token` | PIN entry (`e` = edit, `v` = view) |
| `/admin/modules`, `/admin/library`, `/admin/sources` | Admin pages |

Route `title`s name the page only (`Mission`, `Modules · Admin`); `AppTitleStrategy` appends ` · FPV Radio Map`, and a route without a title shows the app name alone.

## Browser profile

`src/index.html` sets the language (`en`), the description, the dark `theme-color` and `color-scheme`, `noindex` (missions and share links are private), and the icons. `public/manifest.webmanifest` makes the app installable as a standalone app with the same name, colours and icons.

The icons in `public/` (`favicon.svg`, `favicon.ico`, `icons/*.png`) are drawn by `tools/app-icons/build_icons.py` from one description of the mark: a pilot on a ridge under three signal arcs in the signal scale colours. They are committed; after changing the drawing, run `python3 tools/app-icons/build_icons.py` (needs Pillow) from the repository root.
