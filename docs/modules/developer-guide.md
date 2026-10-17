# Writing an rfmap module

A module adds something to the radio map without touching the core: an extra loss term
(rain, foliage, body shadow), an analysis of a route (battery, minimum altitude, relay), a
library kind, or a UI panel. Modules are shared libraries with a C ABI, packed into a signed
`.rfmod` file and installed by the server admin.

The worked example is [`examples/modules/body-shadow`](../../examples/modules/body-shadow).
It builds, packs and passes the server's upload checks in CI.

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
should run your modules; they add it under **Admin → Keys**. Keep `acme.secret` off servers and
out of git: whoever has it can sign as you.

The server has one key built in, the owner's. Modules signed with it run inside the server
process. Modules signed with any other key run in a separate worker process (section 8).
The admin can remove a key at any time; modules signed with it stop loading on the next start.

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
| `version` | SemVer. An upload must be newer than what is installed. |
| `lifecycle` | `alpha`, `beta`, `stable` or `deprecated` (section 8). |
| `abi` | Always `1` for now. |
| `host` | Range of host versions you support, for example `>=0.1.0 <1.0.0`. |
| `requires` / `optional` | Other modules, with version ranges (`^1.2.0`, `>=1.0.0 <2.0.0`, `~1.4.0`, `a || b`). Required ones must be installed first. Optional ones may be missing; check with the host at run time. |
| `provides` | What you add: `loss.contributor`, `route.analysis`, `coverage.analysis`, `library.kind`, `ui.panel`. |
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
- `selftest` runs when the admin uploads the package, in a worker, before anything is
  installed. Check your maths against cases you know the answer to. Return
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

### loss.contributor → `loss.contribute`

Called once per evaluated path. The request:

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
    "samples": [ { "distanceM": 0, "groundMslM": 182.0, "obstacleM": 0.0, "landCover": "grass" } ]
  },
  "options": { }
}
```

`options` holds the per-mission settings the user chose for your module, if your UI offers any.
`landCover` appears only when a land cover layer is configured.

The response:

```json
{
  "terms": [ { "id": "acme.rain/path", "label": "Rain", "valueDb": 0.42, "source": "ITU-R P.838-3" } ],
  "notes": [ { "what": "Rain rate", "detail": "forecast, ±50 % typical" } ]
}
```

Positive `valueDb` is a loss, negative a gain. Every term `id` must start with your module id
and a slash; the server drops terms that do not. Terms appear in the "why" breakdown in the
order you return them.

### route.analysis → `route.analyze`

Called after the core has evaluated every leg of a route.

Request: `{ "mission": <mission.schema.json>, "legs": [<LegResult>], "options": {} }`.

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
`data` is shown under your module id in `RouteResult.modules`.

### coverage.analysis → `coverage.analyze`

Request: `{ "mission": ..., "grid": <CoverageGrid>, "options": {} }`. Response: `{ "reasons": [...],
"layers": [ { "id": "acme.x/layer", "title": "...", "values": [...] } ] }`, with `values` laid
out like the grid's margins.

## 6. Host calls

From `init` or `invoke`: `host().call("method", request)`. Each call returns JSON or throws
`rfmod::HostError`.

| Method | Request | Response |
|---|---|---|
| `geodesy.inverse` | `{from:{lat,lon}, to:{lat,lon}}` | `{distanceM, azimuth1Deg, azimuth2Deg}` on WGS84 (Karney) |
| `geodesy.direct` | `{from:{lat,lon}, azimuthDeg, distanceM}` | `{lat, lon, azimuthDeg}` |
| `elevation.point` | `{lat, lon}` | `{groundMslM, uncertaintyM, source}`; `groundMslM` is null where there is no data |
| `profile.get` | `{from:{lat,lon}, to:{lat,lon}, stepM?}` | `{source, stepM, samples:[{distanceM, lat, lon, groundMslM, obstacleM, uncertaintyM}], missing:[{fromM, toM}]}` |
| `link.evaluate` | `{frequencyHz, txPowerDbm, tx:{lat, lon, aglM, gainDbi, feederLossDb}, rx:{lat, lon, aglM, gainDbi, feederLossDb, sensitivityDbm, requiredMarginDb}, time?}` | `LinkResult` from the OpenAPI file, including every term from other modules |
| `weather.point` | `{lat, lon, time}` | `WeatherPoint` |
| `library.get` | `{kind, id}` with kind `radio-profiles`, `antennas`, `batteries` or `drones` | the library item, including its `version` |

The server never fills missing terrain. `profile.get` reports gaps in `missing`, and
`link.evaluate` fails on a path with gaps; tell the user rather than guessing.

`link.evaluate` from inside `loss.contribute` would recurse and is refused.

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
load only for the admin. Bundles are served from `/api/v1/modules/<id>/ui/...`; `lib/` is never
served.

## 8. Isolation

Modules not signed with the owner key run in `rfmod-worker`, one process per module. The admin
can also run first-party modules this way (`isolateAll`):

- memory capped at `limits.memoryMb` (address space), no core dumps, no file writes,
  32 open files at most;
- started with an empty environment, in the module's own directory, with every server
  descriptor closed;
- `no_new_privs` set, and a fresh network namespace so the module has no network, where the
  kernel allows unprivileged namespaces. The upload report says which of these took effect on
  that server; it is not assumed;
- a call that runs past `limits.timeoutMs` kills the worker; a crash kills only the worker.
  The next call starts a fresh one.

This is a sandbox against mistakes and against greedy code, not a guarantee against a
determined attacker: the module still runs as the server's user and can read what that user
can read. That is why only the admin installs modules and only signed packages are accepted.

Write to stdout or stderr freely; it goes to the server log under your module id. Use
`host().log(level, text)` for messages the admin should see.

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
rfmod check acme.rain-0.1.0.rfmod --trust acme.pub  # every upload check except the self-test
rfmod test  acme.rain-0.1.0.rfmod --trust acme.pub  # also runs the self-test in a worker
# add --with dep-1.0.0.rfmod for each required module
```

`rfmod test` answers host calls only from `--fixtures calls.json` (`{"method": response}`).
`rfmod inspect` lists what is inside a package.

Packing is reproducible: the same files and key give the same bytes.

Send the `.rfmod` to the admin. They upload it under **Admin → Modules**; the server repeats
every check, runs the self-test, and installs only if everything passed. The report lists each
check with its result, so a refusal always says why.

## Package format

A gzip tar with `manifest.json`, your `lib/`, `ui/`, `schema/` and `docs/` files, and:

- `META/files.sha256`: `"<sha256 hex>  <path>\n"` for every other file, sorted by path;
- `META/signature`: `{"alg": "ed25519", "keyId": "<16 hex>", "signature": "<base64>"}` over the
  exact bytes of `META/files.sha256`.

The server refuses links, devices, absolute or `..` paths, duplicate entries, files not
covered by the list, and packages over 64 MB packed or 256 MB unpacked. It re-verifies
installed packages on every start and restores any unpacked file that no longer matches.
