# API notes from the web frontend

Gaps and assumptions found while building `web/` against `docs/api/openapi.yaml`, `schemas/mission.schema.json` and `schemas/library/*.schema.json`. The contract was not changed. Each assumption is marked `@gap` in the code, so it is easy to find.

| # | Where | Gap | What the frontend assumes now |
|---|-------|-----|-------------------------------|
| 1 | Map sources | No endpoint says which tile archives, glyphs and attributions to use. | `GET /config/map` returns `MapSourcesConfig` (`basemap`, `satellite`, `hillshade`, `contours`, `weather`, `glyphs`, `attribution[]`, `center`, `zoom`). If it is missing, the app uses `environment.mapFallback`, which has the tiles-builder names under `/tiles` (`basemap.pmtiles`, `satellite.pmtiles`, `hillshade.pmtiles`, `contours.pmtiles`). The satellite credit is set by the operator, so it should come from this endpoint or from `manifest.json` `attribution_display_text`. |
| 2 | `CoverageGrid` | The bbox order and the origin corner are not stated. | `bbox` = `[west, south, east, north]`, `origin` = north-west corner, and `marginDb` is row-major from north to south. |
| 3 | `JobPart` | There is no `link` field, so a part cannot be matched to a link. | The link is parsed from the part name (`"Coverage grid, control link"`). A `link?: LinkKind` field would remove the guesswork. |
| 4 | `RouteResult.modules` | The value shape per module is open. | `modules[id] = { data, reasons[] }`, as fpv.core returns. Route-level reasons (battery, turn-back) are read from `modules[id].reasons`. |
| 5 | `Fix` / `FixPreviewRow` | The design shows a note under the before/after table and coloured trends. | Optional `Fix.note` and `FixPreviewRow.trend` (`better`, `worse`, `caution`, `neutral`). Without them the UI still works, with neutral rows. |
| 6 | `LinkResult` | There are no endpoint heights (ground MSL and AGL) for the profile chart. | AGL comes from the mission, and ground comes from the profile's first and last samples. |
| 7 | `POST /links/{token}/open`, 401 | The body shape for "tries left" is not given. | `{ message, triesLeft }`. A 429 response carries `Retry-After` in seconds. A 410 response means the link was revoked. |
| 8 | Shared link before the PIN | The mission name cannot be shown on the PIN screen. | The PIN screen shows "Shared mission". A public `GET /links/{token}` that returns `{ role, missionName }` would match the design. |
| 9 | `ShareLink` | The design shows "opened N times" and an expiry. | Not shown. Fields like `openCount` and `expiresAt` would be needed. |
| 10 | `POST /admin/modules` | `install` is named, but its location is not. | Query parameter `?install=true|false`, with the body as `application/octet-stream`. |
| 11 | `/admin/cache`, `/admin/keys` | Bodies are not specified. | Cache: `CacheTier[] = { id, label, limitBytes, usedBytes, hitRate? }`; PUT sends `[{ id, limitBytes }]`. Keys are only listed. |
| 12 | Library 422 | The validation report shape is not given. | `{ message, issues: [{ path, message }] }`. The editor also validates locally with ajv first, using the same schemas. |
| 13 | "Calculate without weather" (stale forecast) | There is no mission field for it. | `mission.modules["rf.weather"]` is removed for that run. |
| 14 | Missing elevation | The region with no data is not described. | The UI names the leg and offers "show the calculated part". A `missingData: { bbox, source }` field would let the map outline it. |
| 15 | Turn-back return leg | There is no explicit representation. | When a turn-back fix is applied, the last waypoint is the turn-back point labelled `T` and `returnToPilot` stays true. The UI draws the return leg itself. |
| 16 | Forecast tier per result | The result does not say which tier (hourly, long-range or none) was used. | The UI derives it from `GET /weather/horizon` and the mission time. |
| 17 | fpv.core compare: coverage difference | There is no server endpoint for a B − A grid. | Per the coordinator, the UI runs a second coverage job for mission B (A plus the merge patch) and subtracts grids in the browser. When the two grids differ in geometry, it samples B at A's cell centres (nearest cell). |
| 18 | fpv.core export | The response gives `{ mediaType, content }` but no file name. | The file name is made from the mission name (`<name>.kml` / `.gpx`). To export, the UI calls `/calc/route` with `modules["fpv.core"].export = [kind]`. That runs a full route calculation just to get a file, so a lighter export call would be better. |
| 19 | fpv.core in `ModuleInfo` | The options schema link is `ModuleInfo.options`. | Compare and export appear only when `fpv.core` is listed with state `active`. Without it, KML/GPX are written in the browser (heights above ground only). |
| 20 | Library: who may edit | The contract puts `admin` security on POST and PUT. | Everyone can browse, read only. Only an admin edits, and `verified` stays an admin decision. Unverified entries and their `source` are shown in the library, the radio setup dialog and the compare picker. |
