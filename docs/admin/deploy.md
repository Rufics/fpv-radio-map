# Deploying FPV Radio Map

For a first install, follow [Production server, step by step](production-setup.md). This page is
the reference for each part.

The stack has four parts, described in [`deploy/docker-compose.yml`](../../deploy/docker-compose.yml):

| Service | What it does |
|---|---|
| `postgis` | PostgreSQL 18 + PostGIS 3.6. Holds missions, share links, sessions, the library and its history, module settings and the result cache. |
| `open-meteo`, `open-meteo-init`, `open-meteo-sync` | Your own Open-Meteo. `open-meteo-init` downloads a first copy of the model data and exits, `open-meteo-sync` then keeps it fresh, and the API answers rfmap-server. No request leaves your network for a forecast. |
| `rfmap-server` | The HTTP API (`/api/v1`). Reads the DEM and land cover COGs directly. |
| `web` | nginx with the built client, `/tiles` (PMTiles over HTTP Range) and a reverse proxy for `/api`. The only service with a published port. |

`postgis`, `open-meteo` and `rfmap-server` sit only on internal networks with no route out.
Only `open-meteo-init` and `open-meteo-sync` can reach the internet, and only `web` can be reached
from outside.

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
   | `OPEN_METEO_MODELS` | The Open-Meteo model rfmap-server asks for (default `dwd_icon`). |
   | `OPEN_METEO_SYNC_MODELS` | The model directories `open-meteo-sync` downloads (default `dwd_icon,dwd_icon_global_eps_native`). See section 4. |

## 2. Start

```sh
cd deploy
docker compose up -d --build
docker compose logs -f rfmap-server
```

At start the server applies the migrations in `/usr/share/rfmap/sql` that the database does
not have yet (once, under an advisory lock). It refuses to start on a database that a newer
version migrated, and it leaves a breaking migration to `rfmap-server --migrate`
([Updates](production-setup.md#11-updates)). It also seeds the reference library from
`data/library/<kind>/seed*.json` (radio profiles, antennas, batteries and drones; see the README
next to each file:
[radio profiles](../../data/library/radio-profiles/README.md),
[antennas](../../data/library/antennas/README.md),
[batteries](../../data/library/batteries/README.md),
[drones](../../data/library/drones/README.md)).
Each seed file is imported once, so later edits by the admin stick; a seed file that a new release
adds is imported at the next start. Seeded entries stay `verified: false` until the admin reviews
them. Check that it is up:

```sh
curl -s http://localhost:8088/api/v1/health
# {"dem":true,"landcover":true,"ok":true,"version":"0.1.0","weather":true}
```

On the first start, `open-meteo-init` downloads about 6 GB of model data (around half an hour
on a slow link). The `open-meteo` API and `open-meteo-sync` start only after it completes
successfully (it retries up to three times), because the API keeps a model grid it failed to
open as failed until it restarts, and `docker compose up -d` does not return until then. Run it in `tmux` or `screen` so a dropped SSH
session does not cut it short, and follow the download from a second shell with
`docker compose logs -f open-meteo-init`. postgis, rfmap-server and web are already running
meanwhile: until the API is up, results note that the forecast is unavailable and use k = 4/3.
If the host reboots before the first pass has finished, Docker does not resume it; run
`docker compose up -d` again. Every later `docker compose up -d` runs the pass again, which only
fetches what changed, and restarts the API after it.

Behind a TLS-intercepting proxy, the image builds need its CA, because CMake fetches
json-schema-validator from GitHub and npm fetches packages. Pass it as a build secret:

```sh
docker build --secret id=build_ca,src=/path/to/proxy-ca.crt -f deploy/server.Dockerfile -t rfmap-server:local ..
```

Put TLS in front of `web` (your reverse proxy or load balancer). Share links and the admin
token must not travel over plain HTTP. Browsers also enable the map's "Find my location" button only
over HTTPS (or on `localhost`).

### Behind a reverse proxy

rfmap-server counts its rate limits per client address (`server.trustProxy`: the last
`X-Forwarded-For` entry, which `web` adds). Behind a proxy, `web` sees only the proxy's address,
so all visitors would share one limit. Tell nginx in `web` which address the proxy connects
from; nginx then takes the visitor's address from the `X-Forwarded-For` the proxy sends, on
connections from that address only. Nothing in the image changes: the setting is one extra
file.

1. Find the address. With `RFMAP_HTTP_PORT=127.0.0.1:8088` and the proxy on the same machine,
   it is the gateway of the `rfmap_public` Docker network:

   ```sh
   docker network inspect rfmap_public --format '{{(index .IPAM.Config 0).Gateway}}'
   ```

   Docker gives its networks addresses in `172.16.0.0/12` by default. Trust that whole range, so
   the setting survives a `docker compose down`, which can give the network a new address. If
   the gateway is outside that range, use the network's subnet
   (`{{(index .IPAM.Config 0).Subnet}}`) instead. Under rootless Docker the proxy may show up
   with yet another address: use the one the first field of a `docker compose logs web` line
   shows for a request through the proxy. For a proxy on another machine, use that machine's
   address, and let only it reach the published port (firewall).

   Trust the Docker range only while `RFMAP_HTTP_PORT` is bound to `127.0.0.1`. If the port is
   published on a public address, visitors that Docker forwards through `docker-proxy` (IPv6
   visitors when the Docker daemon has IPv6 off, which is the default, and every visitor under
   rootless Docker) also arrive from the gateway, and could set their own `X-Forwarded-For`.
2. Write `deploy/nginx-realip.conf`:

   ```nginx
   set_real_ip_from 172.16.0.0/12;
   real_ip_header X-Forwarded-For;
   ```

3. Mount it into `web` with `deploy/docker-compose.override.yml`:

   ```yaml
   services:
     web:
       volumes:
         - ./nginx-realip.conf:/etc/nginx/conf.d/realip.conf:ro
   ```

4. `docker compose up -d`, open the site through the proxy, and check that the lines of
   `docker compose logs web` start with your own address.

The proxy must add the visitor's address as the last `X-Forwarded-For` entry. nginx does that
with `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;`.

The proxy itself must see the visitors' real addresses. Run it on the host, as a system
service, or in a container with `network_mode: host`. A proxy in a container behind published
ports (`80:80`, `443:443`) sees the same `docker-proxy` gateway address for every IPv6 visitor
when the Docker daemon has IPv6 off, and for every visitor under rootless Docker. With an AAAA
record on the domain, all IPv6 visitors would then share one rate limit. If the proxy has to
stay in such a container, enable IPv6 in the Docker daemon and on the proxy's network (see
<https://docs.docker.com/engine/daemon/ipv6/>), or publish no AAAA record. Visitors cannot pick their
address by sending the header themselves: nginx in `web` uses it only on connections from the
trusted addresses, and takes the last entry, which the proxy wrote. Trust only addresses your
proxy uses: anyone who can connect from a trusted address can choose their rate-limit bucket.
With `web` on `127.0.0.1` and the Docker range trusted, that is processes on this machine and
the stack's own containers; rfmap-server, the only other container `web` can see, already takes
`X-Forwarded-For` from whatever connects to it, so the range adds no one new.

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
| `auth.pinConcurrency`, `auth.pinQueue`, `auth.pinWaitMs` | automatic, automatic, 2000 | PIN checks run on the request threads, so they are bounded: that many run at once, that many more wait up to `pinWaitMs` for a slot, and the rest get 503 with `Retry-After` at once. A refused check does not count as a wrong PIN. Automatic is half the CPU cores, at most a quarter of `server.threads`, with a queue of the same size but never more than the spare request threads, so it can be smaller or even zero and one request thread always stays free; each running check at `moderate` cost also takes 256 MiB. Raise them only with cores and `server.threads` to match. |
| `rateLimits.<class>` | see example | Token buckets per client address. Classes: `default`, `open` (PIN attempts and the public link lookup), `create`, `calc`, `coverage`, `admin`. |
| `coverage.maxCells`, `coverage.threads` | 250000, 4 | Larger grids are refused with 422. |
| `calc.maxRouteSamples` | 20000000 | The most terrain samples one `POST /calc/route` may read. Each leg is sampled at the DEM step and every position builds a profile from the pilot, so the work is positions times profile samples; the server works out an upper bound before it starts and refuses a larger route with 422. A sample costs about 1 µs of one core with both links, so the default holds a request thread for at most about 20 s. A 100 km route with a 30 m DEM is around 10 million. |
| `server.maxBodyBytes`, `server.maxUploadBytes` | 1 MiB, 64 MiB | JSON bodies, and flight log files (the only uploads). A JSON body whose `Content-Length` is over `maxBodyBytes` is answered with 413 before it is read, and one sent chunked (without a length) with 411. Compressed request bodies (`Content-Encoding`) are refused with 415 on every route, since the limits count the bytes on the wire, not what they inflate to. The bundled nginx applies the same two limits (`client_max_body_size`); change both together. |
| `cache.*` | 256 MB DEM blocks, 64 MB results in memory, 20000 rows / 1 GB results table | Also changeable at run time under **Admin → Cache** (`PUT /api/v1/admin/cache`). Changes made there are stored and survive a restart. |
| `weather.metaUrl` | empty | The synced model's `meta.json`. With it, results give the forecast run time (`runUtc`). |
| `metrics.enabled` | `false` | Serves `GET /api/v1/admin/metrics` in the Prometheus text format, behind the admin token: request rates and durations, the request, job and cell thread pools, the database pool, the caches, and the process's CPU time, memory, threads and open files. Scrape it with the admin token as bearer credentials. |
| `map` | — | Merged over the map sources `GET /api/v1/config/map` returns. Attribution comes from `manifest.json` next to the DEM. |

## 4. Open-Meteo

[How the weather forecast reaches the pilot](weather-data.md) follows a forecast from these
containers to the app, with diagrams.

rfmap-server asks for these hourly variables at the pilot's location: `temperature_2m`,
`relative_humidity_2m`, `surface_pressure`, `precipitation`, `wind_speed_10m`,
`wind_direction_10m`, and `temperature`, `relative_humidity` and `geopotential_height` at
1000, 975, 950, 925, 900 and 850 hPa. The server builds a refractivity profile from them for the
effective earth radius (`rfext::refractionForPath`), and uses `precipitation` for rain
attenuation (ITU-R P.838-3, 1–1000 GHz only).

The sync service downloads what Open-Meteo stores, not what the API returns, so its variable
list differs from the one above:

- `surface_pressure` is derived by the API from `pressure_msl`, `temperature_2m` and the model
  elevation, and `wind_speed_10m` / `wind_direction_10m` from `wind_u_component_10m` and
  `wind_v_component_10m`. Sync the stored variables.
- ICON global has no 975 hPa level. The API returns nulls for it and the server uses the other
  five levels.
- With `models=dwd_icon`, Open-Meteo 1.7.0 also opens the native ICON grids: `dwd_icon_global_native`
  (sync adds it for `dwd_icon` on its own) and `dwd_icon_global_eps_native` (it does not). Without
  `data/dwd_icon_global_eps_native/static/grid.bin` every `dwd_icon` request fails with
  `{"reason":"Something went wrong.","error":true}`. That is why `OPEN_METEO_SYNC_MODELS` lists
  it. Sync downloads only its static files (about 38 MB), because the variable list names no
  variable that this model stores.

If you change the model, check its directory at `https://openmeteo.s3.amazonaws.com/data/<model>/`
for the variable names it stores. The API hides internal errors behind "Something went wrong.";
the real cause is in `docker compose logs open-meteo`. After you add a model to
`OPEN_METEO_SYNC_MODELS`, run `docker compose up -d`: `open-meteo-init` downloads it, and
compose restarts the API after that.

**Verify on your instance.** After the first sync, run this from a container on the `backend`
network, and check that every variable comes back without nulls:

```sh
docker run --rm --network rfmap_backend curlimages/curl -s \
  "http://open-meteo:8080/v1/forecast?latitude=48.5&longitude=35.2&hourly=temperature_850hPa,geopotential_height_850hPa,surface_pressure,wind_speed_10m&models=dwd_icon"
```

When a variable is missing, the server uses the levels it has. When the forecast does not
cover the requested time, the result says so (`forecastTier: none`) and uses k = 4/3.

## 5. Admin tasks

All under `/api/v1/admin` with `Authorization: Bearer <RFMAP_ADMIN_TOKEN>`, or in the client
under **Admin**.

- **Keys** (`GET /admin/keys`): the trusted signing keys. Adding or removing one is done on
  the server machine: `rfmod trust add author.pub --label "Author" --trust-store
  /var/lib/rfmap/keys.json`, `rfmod trust remove <keyId> ...`, then
  `POST /admin/modules/reload`. A change waits for a running backup, like `rfmod install`.
  The built-in owner key cannot be removed. Removing a key unloads the modules signed with it
  at the next reload.
- **Modules**: installing and removing is a local operation on the server machine, never an
  upload. `rfmod install FILE.rfmod --root /var/lib/rfmap/modules --trust-store
  /var/lib/rfmap/keys.json` checks the archive, signature, manifest, ABI, host range,
  dependencies and lifecycle, runs the self-test in a sandboxed worker and installs only if every
  check passed; `rfmod check FILE.rfmod --trust KEY.pub` only checks; `rfmod remove ID --root ...
  --trust-store ...` takes a module out; `rfmod list --root ... --trust-store ...` shows what is
  installed. The trust store is the server's `modules.trustStore` file, so the CLI sees
  third-party modules as the server does. In the compose deployment: `docker compose exec
  rfmap-server rfmod install /var/lib/rfmap/incoming/FILE.rfmod --root /var/lib/rfmap/modules
  --trust-store /var/lib/rfmap/keys.json` after copying the file into the `rfmap-state` volume. Then
  `POST /admin/modules/reload` (or **Admin → Modules → Reload**) makes the running server pick
  the change up. `PUT /admin/modules/<id>` with `{enabled, options}` disables or configures a
  module, including the built-in `rf.weather`, `rf.clutter`, `rf.uncertainty` and
  `rf.radio-profiles`. The module pack under `modules/fpv` is signed with the development owner
  key in the source tree; re-sign it with your `owner.secret` (`rfmod pack`) before you install
  it on a production server.
- **Dev owner key**: a build without `RFMAP_OWNER_PUBKEY` carries the public development key
  and refuses to start unless `modules.allowDevOwnerKey` is set. Never set it on a server that
  anyone else can reach.
- **Library** (`POST/PUT /library/<kind>`): only the admin edits. Every edit needs the version
  it was made from, saves a new version and keeps the old one in `/history`. Results list the
  item versions they used.
- **Cache** (`/admin/cache`): see the tiers, change limits, clear one tier with
  `DELETE /admin/cache?tier=dem-blocks|results-memory|results-table`, or all of them.

### Module sandbox in containers

Modules not signed with the owner key run in `rfmod-worker` with a memory cap, no file writes,
`no_new_privs`, a Landlock ruleset (read access to the module's own directory and the system
libraries only) and a
seccomp filter that refuses sockets, new processes and signals to other processes. The worker
reports what took effect; a third-party module loads only when everything did
(`modules.requireSandbox`). A separate network namespace needs unprivileged user namespaces,
which Docker's default seccomp profile blocks; the seccomp filter closes the network anyway. In
this compose file, `rfmap-server` and its workers have no route to the internet either, because
both of its networks are internal. They can still reach `postgis` and `open-meteo`. A call that runs past
the module's `limits.timeoutMs` kills its worker. Owner-signed modules run in-process and are
not time-limited.

## 6. Backups and upgrades

- Back up with `docker compose exec -T rfmap-server rfmap-server --backup > rfmap.backup`: one
  file with the database rows, the installed modules and `keys.json`. Everything else can be
  rebuilt. Restoring it is in [Restore on a new server](production-setup.md#101-restore-on-a-new-server).
- Upgrade: `git pull`, then `docker compose up -d --build` from `deploy/` (a pull that moves
  PostgreSQL to a new major needs [a backup and restore](#postgresql-major-upgrades) instead).
  Migrations run at start. Results
  cached under the old version are keyed by input hash, which includes the server version,
  so they are not reused.
- Mission owner keys, share-link tokens and session tokens are stored only as hashes, and
  PINs only as argon2id hashes. A lost owner key cannot be recovered.

### PostgreSQL major upgrades

A new PostgreSQL major version cannot open the data folder of the previous one, so a `git pull`
that moves `postgis` to a new major (16 to 18 is one) moves the data with the server's own
backup and restore (`rfmap-server --backup` and `--restore`) into the new version's empty volume
(`rfmap_pgdata18`). The backup holds data only; the restore builds the schema with the server's
migrations, which also adapt the data to a newer version. The commands are in
[Move to a new PostgreSQL major version](production-setup.md#102-move-to-a-new-postgresql-major-version).
A fresh install needs none of this: it starts on PostgreSQL 18 directly.

## 7. Updating base images

Each `FROM` in the Dockerfiles is pinned by its version tag. Each `image:` in the compose file is
pinned by tag and by the digest of its multi-arch index, read from the registry on 2026-10-17
(`postgis/postgis` on 2026-10-22):

| Image | Tag |
|---|---|
| `ubuntu` | `26.04` (build and runtime of rfmap-server; it matches the GDAL 3.12, libpqxx 7.10 and cpp-httplib 0.26 the code is tested with in CI) |
| `node` | `24-trixie-slim` (client build, Debian 13) |
| `nginx` | `1.28-alpine` |
| `postgis/postgis` | `18-3.6` |
| `ghcr.io/open-meteo/open-meteo` | `1.7.0` |

Dependabot (`.github/dependabot.yml`) opens pull requests for new minor and patch tags of the
Dockerfile base images. It stays on long-term-support lines: Ubuntu LTS, Node.js LTS (even
majors) and nginx stable (even minors). Other major updates are not proposed; move to them by
hand. A tag can be republished with security fixes, so a rebuild picks up the current image for
that tag; rebuild, and run the test suite (`ctest` in a build tree) before you deploy.

To update a compose image by hand, resolve the tag's current digest yourself, for example with
`docker buildx imagetools inspect postgis/postgis:18-3.6`, and replace the digest. Then pull and
recreate the containers from `deploy/` with `docker compose pull && docker compose up -d`;
`docker compose restart` keeps the old image.

The server image and an API smoke test (missions, share link and PIN, `/calc/route`, module
upload) were run against the pins of 2026-10-17. The `postgis/postgis:18-3.6` pin came later, on
2026-10-22, and was not part of that test; run the smoke test once on it. The web image was built on
`node:24-trixie-slim` and served the client on 2026-10-21. The Open-Meteo image could not be
pulled through the test network; build and check it once in your environment.
