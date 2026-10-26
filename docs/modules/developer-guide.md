# Writing an rfmap module

A module adds something to the radio map without touching the core: an extra loss term
(rain, foliage, body shadow), an analysis of a route (battery, minimum altitude, relay), a
library kind, or a UI panel. Modules are shared libraries with a C ABI, packed into a signed
`.rfmod` file and installed by the server operator with the `rfmod` CLI on the server machine.
The HTTP API only lists, enables and configures modules; it cannot add one.

The worked example is [`examples/modules/body-shadow`](../../examples/modules/body-shadow).
It builds, packs and passes the install checks in CI.

## 1. Tools

Build rfmap once (`cmake -B build -G Ninja && cmake --build build`) to get:

- `build/platform/rfmod/rfmod`, the developer tool;
- `build/platform/rfmod/rfmod-worker`, the sandbox the server runs third-party modules in.

The SDK is header-only: `platform/rfmod/include/rfmod/module.hpp` plus `abi.h`. It needs
nlohmann/json 3.11 or newer.

## 2. Your signing key

```sh
rfmod keygen --out acme
```

This writes `acme.pub` and `acme.secret`. Give `acme.pub` to the admin of each server that
should run your modules; they trust it on the server machine with `rfmod trust add`. Keep `acme.secret` off servers and
out of git: whoever has it can sign as you.

The server has one key built in, the owner's. Modules signed with it run inside the server
process. Modules signed with any other key run in a separate worker process (section 8).
The operator can remove a key at any time; modules signed with it stop loading at the next reload.

## 3. Start a project

```sh
rfmod new acme.rain
```

gives `manifest.json`, `schema/options.json`, `src/module.cpp`, `CMakeLists.txt` and a README.

### manifest.json

The full schema is [`schemas/rfmod-manifest.schema.json`](../../schemas/rfmod-manifest.schema.json).
The fields that matter most:

| Field | Meaning |
|---|---|
| `id` | Namespaced, lower case: `acme.rain`. The `fpv.*`, `rf.*` and `example.*` prefixes belong to the owner. |
| `version` | SemVer. An install must be newer than what is installed. |
| `lifecycle` | `alpha`, `beta`, `stable` or `deprecated` (section 8). |
| `abi` | Always `2` for now (the `RFMOD_ABI_VERSION` in `abi.h`). |
| `host` | Range of host versions you support, for example `>=0.1.0 <1.0.0`. |
| `requires` / `optional` | Other modules, with version ranges (`^1.2.0`, `>=1.0.0 <2.0.0`, `~1.4.0`, `a || b`). Required ones must be installed first. Optional ones may be missing; check with the host at run time. |
| `provides` | What you add: `loss.contributor`, `route.preparation`, `route.analysis`, `coverage.analysis`, `flightlog.decoder`, `library.kind`, `ui.panel`. |
| `methods` | Methods you answer, for documentation and the admin page. |
| `entry` | Library per platform: `{"linux-x86_64": "lib/libacme_rain.so"}`. |
| `optionsSchema` | JSON Schema (draft-07) of the options the admin can set. The server validates against it. |
| `ui` | Optional UI bundle and the slots it fills (section 7). Needs `ui.panel` in `provides`. |
| `limits` | `memoryMb` and `timeoutMs` for the worker. The defaults are 512 MB and 30 s. |

The library must report the same `id` and `version` from `manifest()` as `manifest.json` has,
or the server refuses it. The scaffold compiles the manifest in for that reason.

## 4. The module class

```cpp
#include <rfmod/module.hpp>

class RainModule final : public rfmod::Module {
public:
    const char* manifest() const override { return kManifest; }
    void init(const rfmod::HostClient& host, const rfmod::json& options) override;
    rfmod::json invoke(const std::string& method, const rfmod::json& request) override;
    rfmod::json selftest() override;
};

RFMOD_DEFINE_MODULE(RainModule)
```

- `init` gets the admin's options (already validated against your schema). Throwing marks the
  module as failed, with your message on the admin page.
- `invoke` answers methods. Throw `rfmod::UnknownMethod`, `rfmod::BadRequest`, or any
  `std::exception`; the SDK turns them into error codes. Nothing crosses the ABI as a C++
  exception.
- `selftest` runs when the operator installs the package (`rfmod install`), in a sandboxed
  worker, before anything is installed. Check your maths against cases you know the answer to. Return
  `{"ok": bool, "checks": [{"name": "...", "ok": bool, "detail": "..."}]}`. Host calls are not
  available during the self-test.
- `host()` gives the `HostClient` for calls back into the server (section 5).

Calls into one module are serialised: you do not need locks for your own state.

### Units and honesty

All JSON numbers are SI unless the name says otherwise: `...M` metres, `...Hz` hertz,
`...Db` decibels, `...Dbm` dBm, `...Deg` degrees, times in ISO 8601 UTC. Use exact formulas.
If a value rests on data you do not control (a datasheet, a forecast, a survey), say so in a
note rather than folding a guess into the number.

## 5. Contracts

A module runs for a mission only when the mission opts into it: `mission.modules["<id>"]`
holds the per-mission options (`{}` for none). The built-in extensions follow the same rule:
`rf.weather` (forecast refraction and rain) and `rf.clutter` (land cover heights; per-mission
`{"disabledClasses": [50, 80]}` turns WorldCover classes off by code) run only when listed. `rf.uncertainty`
always runs unless the admin disables it. Every module, built-in or installed, can be disabled
by the admin under **Admin → Modules**.

### loss.contributor → `loss.contribute`

Called once per evaluated path, from the transmitter (`tx`) to the receiver (`rx`). The request:

```json
{
  "frequencyHz": 5.8e9,
  "distanceM": 4210.4,
  "rayLengthM": 4210.9,
  "time": "2026-10-18T14:00:00Z",
  "tx": { "lat": 50.45, "lon": 30.52, "groundMslM": 182.0, "antennaAglM": 1.5 },
  "rx": { "lat": 50.48, "lon": 30.55, "groundMslM": 160.0, "antennaAglM": 120.0 },
  "diffraction": { "lineOfSight": true, "nu": -1.31, "lossDb": 0.0, "minFresnelClearance": 0.72 },
  "profile": {
    "source": "Copernicus GLO-30",
    "samples": [ { "distanceM": 0, "lat": 50.45, "lon": 30.52, "groundMslM": 182.0, "obstacleM": 0.0,
                   "uncertaintyM": 4.0, "landCover": "Grassland", "landCoverCode": 30 } ]
  },
  "options": { }
}
```

`options` is `mission.modules["<your id>"]`. `landCover` (the WorldCover class name) and
`landCoverCode` appear only when a land cover layer is configured. `obstacleM` already holds the
clutter height when `rf.clutter` is on; `uncertaintyM` is the DEM's stated vertical error.

The response:

```json
{
  "terms": [ { "id": "acme.rain/path", "label": "Rain", "valueDb": 0.42, "source": "ITU-R P.838-3" } ],
  "notes": [ { "what": "Rain rate", "detail": "forecast, ±50 % typical" } ]
}
```

Positive `valueDb` is a loss, negative a gain. Every term `id` must start with your module id
and a slash; the server drops terms that do not. Terms appear in the "why" breakdown in the
order you return them. Your `notes` go to `LinkResult.uncertainty`, along with a note for each
dropped term. A failure (an error, a timeout, a malformed response) never fails the
calculation: the path is evaluated without your terms, and the failure is listed in
`moduleFailures`.

### route.preparation → `route.prepare`

Called before the core evaluates a route, and only when some waypoint leaves its height to the
server: it has no height in the mission's reference (`aglM`, or `altM` for `"launch"`), or its
`heightSource` is `"auto"` (its stored height is only the last automatic one). Request:

```json
{ "mission": { }, "options": { }, "weather": { }, "time": "2026-10-18T14:00:00Z" }
```

Response: a height for each waypoint you can set, in the mission's reference, and reasons.

```json
{
  "waypoints": [ { "index": 0, "field": "aglM", "heightM": 74, "limitedBy": "control", "blocked": false } ],
  "reasons": [ { "severity": "warn", "leg": 0, "text": "WP1: automatic height 120 m AGL, but ..." } ]
}
```

The server sets only the heights still wanted: a manual height is never changed, and with more
than one such module the first in dependency order wins. Each one set is listed in
`RouteResult.autoHeights` and marked `heightSource: "auto"` in the mission the legs and the
route.analysis modules then see; the stored mission is not changed. Reasons are attached once the
legs exist, the same way as route.analyze's. A failure or a malformed answer (an unknown
waypoint, another field, a height that is not a number) sets nothing and is listed in
`moduleFailures`. A waypoint still without a height after every module makes the calculation a
422.

`fpv.altitude` gives each automatic waypoint the highest corridor floor on its legs: terrain and
obstacles plus 30 m, and with `linkFloor` the height that keeps 60 % of every link's first Fresnel
zone clear, capped by the 120 m AGL ceiling. A leg between two automatic waypoints flies level
over its highest floor; next to a manual waypoint the line from it has to clear the floor. The legs
from and back to the pilot are taken as flown level at the waypoint's height, so the highest floor
along them applies; only the climb from the pilot itself is left out (the take-off ramp and the
climb-out cover it). A
waypoint the ceiling cannot make safe (at the waypoint, or anywhere along one of its legs as the
heights are set), or with no clear Fresnel height up to `maxAglM`, is `blocked`, with a reason that points to `fpv.relay` and `fpv.best-launch`. Its `autoHeight`
option (default true) turns this off.

Its route.analyze also runs a climb assistant (option `climbAssist`, default true). Climbing costs
energy and descending gives nothing back, so the energy-optimal profile is the one with the least
total climb that stays between the corridor's floor and ceiling within the drone's climb rate
(`climbRateMps`, else the drone's measured rate, else 3 m/s) and descent rate (`descentRateMps`,
4 m/s) at the ground speed on each stretch; in the take-off zone the drone climbs and lands
vertically. It is found on a `heightStepM` grid (5 m, coarsened to at most 400 heights a sample) and compared with two simpler strategies:
climbing over launch to the highest floor and flying level, and a steady climb to it and a steady
descent back. Each is turned into the fewest waypoints that follow it within `fitToleranceM` (5 m)
in the mission's reference without crossing the floor or the ceiling (added points are labelled
C1, C2, …; one whose removal keeps the line in the corridor without more climb is dropped, so the
fix may skip a dip of the profile by more than the tolerance) and
offered as a fix that replaces `waypoints`. A stretch takes as long as flying it or as climbing or
descending its height at the drone's rates, whichever is longer; `overCeilingM` counts only where
the floor is under the ceiling. `data.climb` holds the plan's and every
strategy's stats (`riseM`, `timeS`, `minClearanceM`, `overCeilingM`, `steepestClimbMps`, `steepestDescentMps`, and
`energyWh` when the mission has a drone, fetched with `library.get` and flown in the request's
`weather` or, when that is null, the forecast from `weather.point` (still air if the call fails);
only the assistant uses that forecast, the corridor's refraction keeps to the request's `weather`),
the strategies' heights at every corridor sample (`mslM`) and their fixes.
A leg the plan climbs faster than the drone can gets a warning, and a plan that climbs at least
10 m and 10 % more than the energy-optimal profile, or leaves the corridor where that profile does
not, gets an info reason; both carry the energy-optimal fix. A fitted line that breaks either rate
(`withinClimbRate` false, for one when no profile through the corridor keeps to them) is still
shown, but its card offers no fix; for the energy-optimal line the warnings carry no fix, the info
reason is not given, and a note says so.

### route.analysis → `route.analyze`

Called after the core has evaluated every leg of a route. The legs are pilot → wp1 → … → last,
plus last → pilot when `returnToPilot` is true (the default). The core samples each leg at the
DEM's native step. Along a leg the drone's height is interpolated linearly between the two
waypoints in the mission's `altitudeRef`:

- `"launch"`: waypoint heights are `altM`, above the ground at the pilot (a barometric altitude
  from take-off). The drone's MSL is the launch ground plus that height, so it does not follow the
  terrain; its AGL is that MSL minus the DEM ground under it, and a leg where it would be below
  the ground is blocked.
- `"agl"` (and missions without the field): waypoint heights are `aglM`. The drone's MSL is the
  DEM ground plus that height, so it follows the terrain.

The pilot's antenna is at `pilot.aglM` in both. In C++ use `fpv::Route` from `fpv/route.hpp`:
`RoutePoint::heightM` is the height in the mission's reference, `RoutePoint::aglM` is always above
the ground, and `Route::altitude()` converts between them (`fpv/altitude.hpp`). A fix that changes
waypoint heights patches `AltitudeModel::waypointField(mission.altitudeRef)`. `LegResult.worst` holds the worst sample per link, with `sample` saying where it was.

Modules run in dependency order (a module after those it requires or optionally uses), and
each request carries what the earlier ones returned:

```json
{
  "mission": { },
  "legs": [ ],
  "options": { },
  "modules": { "fpv.core": { } },
  "weather": { },
  "time": "2026-10-18T14:00:00Z"
}
```

`options` is `mission.modules["<your id>"]`. `modules` maps the ids of modules that already ran
to their `data`. `weather` is the `WeatherPoint` at the pilot, or null when the mission does not
opt into `rf.weather`. `time` is the time the calculation is for.

Response:

```json
{
  "reasons": [ { "severity": "warn", "text": "Turn back at 3.9 km to land with 20 % left.",
                 "fix": { "id": "battery-turnback", "title": "Add turn-back point",
                          "patch": { },
                          "preview": [ { "label": "Route", "now": "10.85 km", "after": "7.80 km" } ] } } ],
  "data": { }
}
```

`LegResult`, `Reason` and `Fix` are defined in [`docs/api/openapi.yaml`](../api/openapi.yaml).
`patch` is a JSON merge patch to the mission. The client always shows the preview and applies
the patch only when the user confirms, so describe the change completely in `preview`.

- `data` appears under your module id in `RouteResult.modules`.
- A reason with `"leg": <index>` is attached to that leg's reasons. A reason without `leg` is
  route-wide and goes to `RouteResult.reasons`. The server fills in `module` with your id.
- A top-level `battery` object (a `BatteryResult`) next to `data` becomes
  `RouteResult.battery`; the first module to return one wins.
- An error or a malformed response is listed in `RouteResult.moduleFailures`; the legs and
  every other module's output are still returned. A route with failures is not cached.

### coverage.analysis → `coverage.analyze`

Request: `{ "mission": ..., "grid": <CoverageGrid>, "options": {}, "link": "control", "time": "..." }`,
called once per link grid. Response: `{ "reasons": [...], "layers": [ { "id": "acme.x/layer",
"title": "...", "values": [...] } ], "data": {} }`, with `values` laid out like the grid's
`marginDb` (row-major from the north-west corner `origin`, `cellLatDeg` × `cellLonDeg` cells).
The server stores `{layers, data}` under your id in `CoverageGrid.modules`.

### flightlog.decoder → `flightlog.probe`, `flightlog.decode`

A decoder turns one flight log format (Betaflight Blackbox, DJI, ...) into the normalized flight
log, [`schemas/flightlog.schema.json`](../../schemas/flightlog.schema.json) (`flightlog/v1`).
It only reads the file: the server's `rf-flightlog` core turns the normalized log into the
replay, the story and plan against fact, the same way for every format. A decoder always runs
isolated (section 8), even when it is signed with the owner key, because it parses files that
users upload.

Both methods take `{ "file": "<base64>", "fileName": "LOG00007.BFL" }`.

- `flightlog.probe` gets only the first 64 KiB of the file and answers
  `{ "match": true|false, "format": "...", ... }`. Tell the format from those bytes. The server
  asks each installed decoder in turn and uses the first that matches.
- `flightlog.decode` gets the whole file and answers the normalized document, one entry in
  `logs` per flight in the file. A flight with no data (only a header, an unsupported firmware) is still an entry, with
  its `status` and a `notes` entry that says why and what to do. Read the format as the
  firmware writes it, per version, and say in `firmware.supported` when a version is outside
  what you know. Never guess a field you could not read: leave it out, so the UI shows "not
  logged" rather than a wrong number. A file you cannot read is a `BadRequest` (the user sees
  422 with your message); the server checks the answer against the schema and refuses a
  document that does not match.

## 6. Host calls

From `init` or `invoke`: `host().call("method", request)`. Each call returns JSON or throws
`rfmod::HostError`.

| Method | Request | Response |
|---|---|---|
| `geodesy.inverse` | `{from:{lat,lon}, to:{lat,lon}}` | `{distanceM, azimuth1Deg, azimuth2Deg}` on WGS84 (Karney) |
| `geodesy.direct` | `{from:{lat,lon}, azimuthDeg, distanceM}` | `{lat, lon, azimuthDeg}` |
| `elevation.point` | `{lat, lon}` | `{groundMslM, uncertaintyM, source}`; `groundMslM` is null where there is no data |
| `profile.get` | `{from:{lat,lon}, to:{lat,lon}, stepM?}` (default: the DEM's native step) | `{source, stepM, samples:[{distanceM, lat, lon, groundMslM, obstacleM, uncertaintyM?}], missing:[{fromM, toM}]}`; `groundMslM` is null on samples without data |
| `link.evaluate` (mission form) | `{mission, link: "control" \| "video", drone:{lat, lon, aglM}, ground?:{lat, lon, aglM}}` | `LinkResult` for that link of the mission, between `ground` (default `mission.pilot`) and `drone` |
| `link.evaluate` (raw form) | `{frequencyHz, txPowerDbm, tx:{lat, lon, aglM, gainDbi, feederLossDb}, rx:{lat, lon, aglM, gainDbi, feederLossDb, sensitivityDbm, requiredMarginDb}, time?}` | `LinkResult` from the OpenAPI file, including every term from other modules |
| `weather.point` | `{lat, lon, time}` | `WeatherPoint` |
| `library.get` | `{kind, id}` with kind `radio-profiles`, `antennas`, `batteries` or `drones` | the library item, including its `version` |

The server never fills missing terrain. `profile.get` reports gaps in `missing`, and
`link.evaluate` fails on a path with gaps with an error starting `missing-data:`; tell the user
rather than guessing.

The mission form of `link.evaluate` takes the mission document itself, which need not be
saved: send a patched copy to try a change before you propose it as a `Fix`. The server
validates it against `mission.schema.json` (an error starting `bad_request:` names the first
problem), resolves `links[link]` and the library items from the document, and uses its
`time`, `modules` and options, exactly as `/calc/route` would. During a calculation, the raw
form uses that calculation's time and opted-in modules unless you pass `time`.

`link.evaluate` from inside `loss.contribute` would recurse and is refused, and host calls
nest at most four deep.

## 7. UI bundles

A module with `"provides": [..., "ui.panel"]` can ship `ui/<name>.js`, an ES module the web
client imports at run time. It never sees the server's internals: it gets a read-only view of
the mission and its result, a number formatter, and the slots it declared in `ui.slots`.

```js
export const kit = '1';            // major version of the ui-kit contract
export function register(api) {
  api.slots.register({
    id: 'battery-panel',
    slot: 'side.panels',             // header.actions, side.panels, map.layers, map.overlays, bottom.drawer, verdict.reasons
    title: 'Battery',
    mount(host, ctx) {
      const off = api.state.subscribe(({ result }) => {
        const b = result?.modules?.['acme.battery'];
        host.textContent = b ? api.format.format(b.usableWh, 'Wh') : 'No route yet';
      });
      return () => off();            // called when the module is unloaded
    },
  });
}
```

The full contract is `web/projects/ui-kit/src/lib/modules/module-api.ts`. A bundle that throws
is unloaded and its slots are cleared; the rest of the page carries on. Alpha modules' bundles
load only for the admin: `GET /modules` omits their `ui` for everyone else, and the admin's
listing carries a signed `?grant=` (valid 12 h) on the bundle URL, since a browser imports
bundles without an Authorization header. Bundles are served from `/api/v1/modules/<id>/ui/...`
through the registry's public-file lookup; `lib/` is never served.

## 8. Isolation

Modules not signed with the owner key run in `rfmod-worker`, one process per module. The
operator can also run first-party modules this way (`isolateAll`):

- memory capped at `limits.memoryMb` (address space) and the call time at `limits.timeoutMs`,
  both further capped by the server's `modules.maxMemoryMb` and `modules.maxTimeoutMs`; a
  reply larger than 16 MiB ends the worker, and a log line is cut at 1 MiB;
- no core dumps, no file writes, 32 open files at most;
- started with an empty environment, in the module's own directory, with every server
  descriptor closed; the host kills the worker's whole process group;
- `no_new_privs` set;
- a Landlock ruleset that leaves the module read access to its own directory and the system
  library directories only, so it can read neither the server's files nor its configuration;
- a seccomp filter that refuses sockets, new processes (`fork`, `clone` without
  `CLONE_THREAD`, `execve`), `ptrace`, the mount and module calls, `setuid` and the like,
  changes to file modes, owners, times and attributes, truncation, and signals to any process
  but itself (threads are allowed);
- a fresh user and network namespace where the kernel allows unprivileged namespaces.

The worker confines itself before it loads the module's library, so the library's
constructors already run inside. It reports which of these took effect; the host assumes none. A module that is not
signed with the owner key loads only when all of them did (`modules.requireSandbox`, on by
default), so a kernel without Landlock or seccomp runs first-party modules only. Owner-signed
modules run in-process and cannot be stopped mid-call, so keep their calls short.

This is a sandbox against mistakes and against greedy code; a module still runs as the
server's user. That is why only the operator installs modules, only signed packages are
accepted, and the server's own module ids (`rf.*`, `fpv.*`) need the owner key.

Write to stdout or stderr freely; it goes to the server log under your module id. Use
`host().log(level, text)` for messages the operator should see.

Host calls are answered only while the server evaluates a request: a module that calls the host
from `init` or from a thread of its own gets an error. Each request allows 10 000 host calls in
all, nested at most 4 deep, and `link.evaluate` is refused inside `loss.contribute`, where it
would run the caller again. Every request field is checked for shape and range before it reaches
a service, and the error names the field.

## 9. Versions and deprecation

Follow SemVer: break the request or response shape only in a new major version. Dependents pin
ranges, and the server refuses an upgrade that would leave an enabled dependent out of its
range; the dependent's author releases a matching version first, or the admin disables it.

To retire a module or a major version, Kubernetes style:

1. Release a version with `"lifecycle": "deprecated"` and a `deprecation` block:
   `{"since": "1.6.0", "removedIn": "1.9.0", "replacement": "acme.rain2", "message": "..."}`.
   `removedIn` must be at least three minor releases after `since`, or a later major.
2. The admin page and every result that used the module show your message, and modules that
   require yours are told they depend on a deprecated module.
3. From host version `removedIn` the server stops loading it, and dependents report
   `missing-dependency` with the reason.

## 10. Pack, check, test, ship

```sh
cmake -B build -DRFMOD_SDK=/path/to/fpv-radio-map/platform/rfmod/include && cmake --build build
mkdir -p stage/lib && cp manifest.json stage/ && cp -r schema stage/ && cp build/libacme_rain.so stage/lib/
rfmod pack stage --key acme.secret                  # → acme.rain-0.1.0.rfmod
rfmod check acme.rain-0.1.0.rfmod --trust acme.pub  # every install check except the self-test
rfmod test  acme.rain-0.1.0.rfmod --trust acme.pub  # also runs the self-test in a worker
# add --with dep-1.0.0.rfmod for each required module
```

`rfmod test` answers host calls only from `--fixtures calls.json` (`{"method": response}`).
It installs the packages into a private directory (`.rfmod-test-XXXXXX`, mode 0700) under the
current directory and removes it when it finishes.
`rfmod inspect` lists what is inside a package.

Packing is reproducible: the same files and key give the same bytes.

Send the `.rfmod` to the operator. They copy it to the server machine and run
`rfmod trust add acme.pub --label "Acme" --trust-store /var/lib/rfmap/keys.json` once, then
`rfmod install acme.rain-0.1.0.rfmod --root /var/lib/rfmap/modules --trust-store /var/lib/rfmap/keys.json`;
the CLI repeats every check,
runs the self-test in a sandboxed worker and installs only if everything passed, then
`POST /api/v1/admin/modules/reload` makes the server pick it up. The report lists each check
with its result, so a refusal always says why. There is no upload through the API or the
browser: a module reaches a server only through someone who can log in to its machine.

## Package format

A gzip tar with `manifest.json`, your `lib/`, `ui/`, `schema/` and `docs/` files, and:

- `META/files.sha256`: `"<sha256 hex>  <path>\n"` for every other file, sorted by path;
- `META/signature`: `{"alg": "ed25519", "keyId": "<16 hex>", "signature": "<base64>"}` over the
  exact bytes of `META/files.sha256`.

The server refuses links, devices, absolute or `..` paths, duplicate entries, files not
covered by the list, and packages over 64 MB packed or 256 MB unpacked. It re-verifies
installed packages on every start and restores any unpacked file that no longer matches.
