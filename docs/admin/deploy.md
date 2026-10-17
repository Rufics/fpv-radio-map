# Deploying FPV Radio Map

The stack has four parts, described in [`deploy/docker-compose.yml`](../../deploy/docker-compose.yml):

| Service | What it does |
|---|---|
| `postgis` | PostgreSQL 16 + PostGIS 3.4. Holds missions, share links, sessions, the library and its history, module settings and the result cache. |
| `open-meteo` + `open-meteo-sync` | Your own Open-Meteo. The sync service downloads model runs; the API answers rfmap-server. No request leaves your network for a forecast. |
| `rfmap-server` | The HTTP API (`/api/v1`). Reads the DEM and land cover COGs directly. |
| `web` | nginx with the built client, `/tiles` (PMTiles over HTTP Range) and a reverse proxy for `/api`. The only service with a published port. |

`postgis`, `open-meteo` and `rfmap-server` sit only on internal networks with no route out.
Only `open-meteo-sync` can reach the internet, and only `web` can be reached from outside.

## 1. Before you start

1. **Tiles.** Run [`tools/tiles-builder`](tiles-builder.md) for your area. Its output directory
   (`dem.cog.tif`, `landcover.cog.tif`, `*.pmtiles`, `fonts/`, `manifest.json`) is mounted
   read-only into `rfmap-server` at `/data` and into `web` at `/tiles`. nginx refuses `*.tif`
   under `/tiles`; the COGs are for calculations only.
2. **DEM error.** Find the vertical error your DEM provider states, and where it is stated.
   Without it, results carry no terrain error and say so. Do not estimate it.
3. **Owner key.** Create it once on a machine that is not the server:

   ```sh
   build/platform/rfmod/rfmod keygen --out owner
   ```

   `owner.pub` goes into the image at build time. Modules signed with `owner.secret` run inside
   the server process, so keep the secret offline. Without a key, the image is built with the
   public development key, and the server logs a warning at every start
   (`RFMAP_OWNER_KEY_IS_DEV=1`): anyone could sign in-process modules for it. Never run that in
   production.
4. **Secrets.** Copy `deploy/.env.example` to `deploy/.env` (ignored by git) and fill it in:

   | Variable | Value |
   |---|---|
   | `POSTGRES_PASSWORD` | Random. |
   | `RFMAP_ADMIN_TOKEN` | At least 24 random characters, for example `openssl rand -base64 33`. Compared in constant time. Empty disables the admin API. |
   | `RFMAP_OWNER_PUBKEY` | Contents of `owner.pub`. |
   | `RFMAP_TILES_DIR` | Absolute path of the tiles-builder output. |
   | `RFMAP_DEM_STATED_ERROR_M`, `RFMAP_DEM_STATED_ERROR_SOURCE` | From step 2. |
   | `RFMAP_HTTP_PORT` | Port `web` listens on (default 8088). |
   | `OPEN_METEO_MODELS` | The Open-Meteo model to sync and to ask for (default `dwd_icon`). |

## 2. Start

```sh
cd deploy
docker compose up -d --build
docker compose logs -f rfmap-server
```

At start the server applies the SQL migrations in `/usr/share/rfmap/sql` (once, under an
advisory lock) and seeds the radio profiles from `data/library/radio-profiles/seed.json` once.
Seeded entries stay `verified: false` until the admin reviews them. Check that it is up:

```sh
curl -s http://localhost:8088/api/v1/health
# {"dem":true,"landcover":true,"ok":true,"version":"0.1.0","weather":true}
```

The first forecast is ready once `open-meteo-sync` has downloaded a run, which takes a while.
Until then, results state that no forecast covers the time and use k = 4/3.

Behind a TLS-intercepting proxy, the image builds need its CA, because CMake fetches
json-schema-validator from GitHub and npm fetches packages. Pass it as a build secret:

```sh
docker build --secret id=build_ca,src=/path/to/proxy-ca.crt -f deploy/server.Dockerfile -t rfmap-server:local ..
```

Put TLS in front of `web` (your reverse proxy or load balancer). Share links and the admin
token must not travel over plain HTTP.

## 3. Configuration

The server reads `/etc/rfmap/rfmap.json`. Compose mounts
[`server/config/server.example.json`](../../server/config/server.example.json), which documents
every key with its default. To change something, either edit a copy and mount that, or set an
environment variable:

- `RFMAP__<SECTION>__<KEY>=value` sets `config[section][key]`. Names are matched without
  regard to case, and values are parsed as JSON when they parse, for example
  `RFMAP__CACHE__DEMBLOCKSMB=512` or `RFMAP__WEATHER__TIERS='[{"name":"hourly","models":"dwd_icon"}]'`.
- Short forms: `RFMAP_DATABASE_URL`, `RFMAP_ADMIN_TOKEN`, `RFMAP_PORT`, `RFMAP_OPEN_METEO_URL`,
  `RFMAP_DEM`, `RFMAP_LANDCOVER`.

Settings you will most likely touch:

| Key | Default | Notes |
|---|---|---|
| `server.trustProxy` | `true` in the example | Takes the last `X-Forwarded-For` hop as the client address. Set it only when a proxy you run is in front (in compose, `web` is). |
| `auth.pinMaxAttempts`, `auth.pinLockoutMinutes` | 5, 15 | After that many wrong PINs the link pauses (429 with `Retry-After`). Each further lockout of the same link doubles the pause. A correct PIN resets the count. |
| `auth.argon2Cost` | `moderate` | PIN hashing (argon2id, libsodium). `interactive` is cheaper; `sensitive` costs about 1 GiB per hash. |
| `rateLimits.<class>` | see example | Token buckets per client address. Classes: `default`, `open` (PIN attempts and the public link lookup), `create`, `calc`, `coverage`, `admin`. |
| `coverage.maxCells`, `coverage.threads` | 250000, 4 | Larger grids are refused with 422. |
| `cache.*` | 256 MB DEM blocks, 64 MB results in memory, 20000 rows / 1 GB results table | Also changeable at run time under **Admin → Cache** (`PUT /api/v1/admin/cache`). Changes made there are stored and survive a restart. |
| `weather.metaUrl` | empty | The synced model's `meta.json`. With it, results give the forecast run time (`runUtc`). |
| `map` | — | Merged over the map sources `GET /api/v1/config/map` returns. Attribution comes from `manifest.json` next to the DEM. |

## 4. Open-Meteo

rfmap-server asks for these hourly variables at the pilot's location: `temperature_2m`,
`relative_humidity_2m`, `surface_pressure`, `precipitation`, `wind_speed_10m`,
`wind_direction_10m`, and `temperature`, `relative_humidity` and `geopotential_height` at
1000, 975, 950, 925, 900 and 850 hPa. The sync service in compose downloads exactly these for
`OPEN_METEO_MODELS`. The server builds a refractivity profile from them for the effective
earth radius (`rfext::refractionForPath`), and uses `precipitation` for rain attenuation
(ITU-R P.838-3, 1–1000 GHz only).

**Verify on your instance.** The model name (`dwd_icon`), the sync command line and the
pressure-level variable names follow the Open-Meteo documentation but have not been run
against a real instance here. After the first sync, run this from a container on the
`backend` network, and check that every variable comes back without nulls:

```sh
curl "http://open-meteo:8080/v1/forecast?latitude=48.5&longitude=35.2&hourly=temperature_850hPa,geopotential_height_850hPa,surface_pressure&models=dwd_icon"
```

When a variable is missing, the server uses the levels it has. When the forecast does not
cover the requested time, the result says so (`forecastTier: none`) and uses k = 4/3.

## 5. Admin tasks

All under `/api/v1/admin` with `Authorization: Bearer <RFMAP_ADMIN_TOKEN>`, or in the client
under **Admin**.

- **Keys** (`/admin/keys`): add a module author's `.pub`; remove it again. The built-in owner
  key cannot be removed. Removing a key unloads the modules signed with it at once.
- **Modules** (`POST /admin/modules?install=true|false`, body `application/octet-stream`):
  the server checks the archive, signature, manifest, ABI, host range, dependencies and
  sandbox, runs the self-test in a worker, and installs only if every check passed.
  `?install=false` only checks. `PUT /admin/modules/<id>` with `{enabled, options}` disables
  or configures a module, including the built-in `rf.weather`, `rf.clutter`, `rf.uncertainty`
  and `rf.radio-profiles`. The module pack under `modules/fpv` is signed with the development
  owner key in the source tree; re-sign it with your `owner.secret` (`rfmod pack`) before you
  upload it to a production server.
- **Library** (`POST/PUT /library/<kind>`): only the admin edits. Every edit needs the version
  it was made from, saves a new version and keeps the old one in `/history`. Results list the
  item versions they used.
- **Cache** (`/admin/cache`): see the tiers, change limits, clear one tier with
  `DELETE /admin/cache?tier=dem-blocks|results-memory|results-table`, or all of them.

### Module sandbox in containers

Modules not signed with the owner key run in `rfmod-worker` with a memory cap, no file writes
and `no_new_privs`. A separate network namespace needs unprivileged user namespaces, which
Docker's default seccomp profile blocks. The upload report then says
`network NOT isolated on this host`; this was seen in a test of this image. In this compose
file, `rfmap-server` and its workers have no route to the internet anyway, because both of its
networks are internal. They can still reach `postgis` and `open-meteo`. A call that runs past
the module's `limits.timeoutMs` kills its worker. Owner-signed modules run in-process and are
not time-limited.

## 6. Backups and upgrades

- Back up the `pgdata` volume (or `pg_dump rfmap`) and the `rfmap-state` volume (installed
  modules and `keys.json`). Everything else can be rebuilt.
- Upgrade: `git pull`, then `docker compose up -d --build`. Migrations run at start. Results
  cached under the old version are keyed by input hash, which includes the server version,
  so they are not reused.
- Mission owner keys, share-link tokens and session tokens are stored only as hashes, and
  PINs only as argon2id hashes. A lost owner key cannot be recovered.

## 7. Updating base images

Every `FROM` and `image:` is pinned by the digest of its multi-arch index, read from the
registry on 2026-10-17:

| Image | Tag |
|---|---|
| `ubuntu` | `24.04` (build and runtime of rfmap-server; it matches the GDAL 3.8, libpqxx 7.8 and cpp-httplib 0.14 the code is tested with) |
| `node` | `24-bookworm-slim` (client build) |
| `nginx` | `1.27-alpine` |
| `postgis/postgis` | `16-3.4` |
| `ghcr.io/open-meteo/open-meteo` | `1.7.0` |

Dependabot (`.github/dependabot.yml`) opens pull requests for new digests and for minor and
patch tags. For base images it stays on long-term-support lines: Ubuntu LTS, Node.js LTS (even
majors) and nginx stable (even minors). Other major updates are not proposed; move to them by
hand.

To update one by hand, resolve the tag's current digest yourself, for example with
`docker buildx imagetools inspect node:24-bookworm-slim`. Replace the digest, rebuild, and run
the test suite (`ctest` in a build tree) before you deploy.

The server image and an API smoke test (missions, share link and PIN, `/calc/route`, module
upload) were run against these pins on 2026-10-17. The web image could not be built then
because Docker Hub rate-limited the pull, and the Open-Meteo image could not be pulled through
the test network. Build and check both once in your environment.
