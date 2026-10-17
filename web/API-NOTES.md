# API notes from the web frontend

Gaps and assumptions found while building `web/` against `docs/api/openapi.yaml`, `schemas/mission.schema.json` and `schemas/library/*.schema.json`. The contract was not changed by the frontend. The few assumptions that are still open are marked `@gap` in the code.

`docs/api/openapi.yaml` is now the final contract, and `web/` follows it. Most of the gaps listed in the first round are closed. The table keeps their numbers so earlier references still work.

## Still open

| # | Where | Gap | What the frontend does |
|---|-------|-----|------------------------|
| 5 | `Fix` / `FixPreviewRow` | The design shows a note under the before/after table and coloured trends. | Reads the optional `Fix.note` and `FixPreviewRow.trend` (`better`, `worse`, `caution`, `neutral`) when present. Without them the UI still works, with neutral rows. These are `@gap` extensions in `api.models.ts`. |
| 15 | Turn-back | The route now carries the return leg (see below), but a turn-back fix has no representation of its own. | A turn-back fix makes the turn-back point the last waypoint (labelled `T`), and `returnToPilot` stays true. The server's return leg T → pilot is then drawn and listed as "Turn back · X km". |
| 17 | fpv.core compare: coverage difference | `/calc/coverage` takes a mission only, so B's grid cannot be requested on A's exact bbox and cell size. | The grids are never resampled. The difference is shown only for cells that coincide exactly: same `cellLatDeg`/`cellLonDeg`, with origins a whole number of cells apart. The other cells are hatched grey and labelled "not comparable", and the count is shown in the compare panel. When B moves the pilot to a latitude where the cell degrees differ, nothing is comparable and the UI says so instead of drawing a map. An optional `grid: { bbox, cellLatDeg, cellLonDeg }` on `/calc/coverage` would allow a full map for every B. |
| 18 | fpv.core export | Exporting runs a full `/calc/route` with `modules["fpv.core"].export = [kind]` just to get a file. | Works as is. A lighter export call would be cheaper. The file name is made from the mission name (`<name>.kml` / `.gpx`). |

## Closed by the final contract

| # | Where | Was | Now |
|---|-------|-----|-----|
| 1 | Map sources | No endpoint for tiles, glyphs and attribution. | `GET /config/map` as defined. `center` is `[lon, lat]`. `environment.mapFallback` is used only when the call fails. Inline GeoJSON and XYZ templates remain client extensions, used by the dev mock only. |
| 2 | `CoverageGrid` | Bbox order and origin corner were not stated. | `bbox` = `[west, south, east, north]`, `origin` = north-west corner, `marginDb` row-major from north to south, regular in degrees (`cellLatDeg`, `cellLonDeg`). |
| 3 | `JobPart` | No `link` field. | `JobPart.link` is used. The part name is parsed only as a fallback (`partLink()` in `mission-store.ts`). |
| 4 | `RouteResult.modules` | The value shape per module was open. | `modules[id]` is the module's `data` object itself. Route-wide reasons are in `RouteResult.reasons`, each with `module`. A module reason with `leg` is in that leg's `reasons`. Reasons carry `link`, and the verdict picks the reason for the failing link. |
| 6 | `LinkResult` heights | No endpoint heights for the profile chart. | `LinkResult.ends` (`ground`/`air` with `aglM`, `groundMslM`, `antennaMslM`, and `transmitter`). The profile and the diffraction distances run from the transmitter (drone → pilot for video). `groundToAir()` in the ui-kit flips them so the chart always has the pilot on the left. |
| 7 | `/links/{token}/open` errors | Body shapes for 401 and 429 were not given. | 401 `{ message, triesLeft }`; 429 `{ message, retryAfterS }` plus `Retry-After` (body first, then header); 410 = revoked or expired. |
| 8 | Shared link before the PIN | The mission name was not available. | Public `GET /links/{token}` → `{ role, missionName, expiresAt?, pausedUntil? }`. The PIN screen shows the name, role and expiry. It shows the pause straight away from `pausedUntil`, the "no longer works" state on 410 (saying "expired" when it can tell), and the pause from `retryAfterS` on 429. |
| 9 | `ShareLink` | No open count or expiry. | The share dialog shows `openCount`, `expiresAt` and `pausedUntil` for each link, and can create a link with an optional `expiresAt` (1, 7 or 30 days, or until revoked). |
| 10 | `POST /admin/modules` | Where `install` goes was not stated. | Query `?install=true|false`, with the body as `application/octet-stream`. |
| 11 | `/admin/cache`, `/admin/keys` | Bodies were not specified. | Cache tiers `dem-blocks`, `results-memory` and `results-table` (`limitRows` for the table only). All fields except `id` are optional and shown only when present. PUT sends `[{ id, limitBytes?, limitRows? }]` and shows the returned tiers. `DELETE /admin/cache?tier=` is used for "Clear". Keys: list, add `{ key, label }`, and delete by id. The built-in key has no Remove button. Module enable/disable (`PUT /admin/modules/{id}`) and remove (`DELETE`) are on the modules page, and `stateDetail` is shown. |
| 12 | Library 422 and versions | The report shape was not given. | `issues[].path` is a JSON pointer into the body sent, and is shown as such. PUT sends the `version` that was edited. On 409, the editor says nothing was saved and offers to load the current version. After saving, the editor shows the version the server returned. |
| 13 | "Calculate without weather" | There was no mission field for it. | Leaving `rf.weather` out of `mission.modules` is the switch, and the result then says `forecastTier: "off"`. |
| 14 | Missing elevation | The region was not described. | `LegResult.missingData { bbox, source }` is outlined on the map, dashed. Each drone position in `LegResult.missing[]` is drawn as a point, and `samples` gives the "N of M positions not calculated" line. The 422 missing-data body of `/calc/path` and `/elevation/profile` is parsed by `ApiError.missingData` (same `MissingDataLayer` shape), but the app's flows do not call those two endpoints at present. |
| 16 | Forecast tier | The UI derived the tier itself. | `RouteResult.forecastTier` (`hourly`, `long-range`, `none`, `off`) is read directly. |
| 19 | fpv.core in `ModuleInfo` | — | Unchanged: compare and export appear only when `fpv.core` is listed as `active`. Without it, KML/GPX are written in the browser (heights above ground only). |
| 20 | Library: who may edit | — | Unchanged: everyone can browse read-only, and only an admin edits or marks entries verified. `verified: false` and the `source` are shown wherever an entry is picked. |

## Facts the UI relies on (read from the server source)

- `LegResult.index` is 0-based. The UI numbers legs from 1 for display.
- With `returnToPilot` (default true), `legs[]` has one more leg: last waypoint → pilot. It is listed as "B → A, return", drawn as the return line, and can be clicked like any other leg.
- Leg `from`/`to` are the point labels, or "Pilot"/"WPn".
- A leg's `worst` link carries `sample { lat, lon, aglM, alongLegM }`, which is where along the leg it was taken. The why drawer says "at B" or "X km into the leg".
