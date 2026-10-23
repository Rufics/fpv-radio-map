# Production server, step by step

This guide takes one Linux machine from nothing to a running FPV Radio Map server with your own
map data. It puts the steps from [Building map data](tiles-builder.md) and
[Deployment](deploy.md) in the order you run them, with the commands to copy. Those two pages
have every option and the reasons behind them.

You will:

1. build the map data for your area: basemap, terrain, land cover and, optionally, satellite
   imagery;
2. create the owner key and the secrets;
3. start the Docker stack and check it;
4. sign and install the FPV modules;
5. fill the equipment library;
6. put TLS in front, and set up backups and updates.

All commands run in a terminal on the server, as your normal user. They use four directories
in your home: `~/fpv-radio-map` (the code), `~/rfmap-data` (map data), `~/rfmap-keys` and
`~/rfmap-modules`. Sizes and times marked **(estimate)** depend on your area and hardware.

## 0. What you need

| | |
|---|---|
| Machine | Linux (x86-64 or arm64) with Docker Engine and the Compose plugin. 4 cores and 8 GB RAM are enough for a district. A country basemap needs 8–16 GB RAM **(estimate)**. |
| Disk | About 5 GB for the images, plus the map data: a few hundred MB for a district, tens of GB for a country with satellite imagery **(estimate)**. See [disk, RAM and time](tiles-builder.md#7-disk-ram-and-time--all-estimate). |
| Network | Outbound HTTPS while you build: Docker Hub, GitHub and its container registry, Ubuntu and npm mirrors, and the map data sources (Geofabrik, AWS Open Data, osmdata.openstreetmap.de, naciscdn.org, and your satellite mosaic's host, such as tiles.maps.eox.at). At run time only the Open-Meteo sync services go out: `open-meteo-init` during start-up and `open-meteo-sync` afterwards, both to `openmeteo.s3.amazonaws.com`. |
| Public server | A domain name pointing at the machine, and ports 80 and 443 open. |

Install Docker from <https://docs.docker.com/engine/install/>, then let your user run it and log
in again:

```sh
sudo usermod -aG docker "$USER"
```

Check both parts:

```sh
docker info --format '{{.ServerVersion}}'   # prints a version, not an error
docker compose version                      # Docker Compose version v2.17.0 or later
```

Docker Desktop on Windows or macOS is fine for trying it out (Settings → Resources: 8 GB RAM or
more). Run a public server on Linux.

## 1. Get the code

```sh
cd ~
git clone https://github.com/Rufics/fpv-radio-map.git
```

## 2. Build the map data

Every map layer is a file that `rfmap/tiles-builder` writes into one output directory. The server
and the browser read only that directory, so at run time nothing comes from a third-party map
service, and **no API keys or accounts are needed** for any built-in source.

### 2.1 The layers

| Layer | Builder step | Files in `out/` | Source (default) | Used by | Needed? |
|---|---|---|---|---|---|
| Basemap (roads, places, water, buildings) | `basemap` | `basemap.pmtiles` | OpenStreetMap extract from Geofabrik | browser | yes |
| Terrain for calculations | `dem` | `dem.cog.tif` | Copernicus DEM GLO-30 (AWS Open Data) | rfmap-server | yes |
| Hillshade | `dem` | `hillshade.pmtiles` | derived from `dem.cog.tif` | browser | recommended |
| Contour lines | `dem` | `contours.pmtiles` | derived from `dem.cog.tif` | browser | recommended |
| Land cover (clutter) | `landcover` | `landcover.cog.tif` | ESA WorldCover 10 m (AWS Open Data) | rfmap-server | recommended: without it the server starts, logs a warning and leaves clutter out |
| Satellite imagery | `satellite` | `satellite.pmtiles` | none: you choose a mosaic and check its licence | browser | optional |
| Labels and credits | every step | `fonts/`, `manifest.json` (and `style-dark.json` for other clients) | the builder | browser, rfmap-server | yes |

[How the tiles builder generates map data](map-generation.md) shows what each step copies,
converts or derives, and the formats it reads and writes.

`all` runs `basemap`, `dem` and `landcover`, and `satellite` as well when you give it a satellite
source. Without one it logs `skipping satellite` and goes on. This guide runs satellite as a
separate step, because it is the one you choose and time yourself.

### 2.2 Build the image and choose the area

The map data lives outside the repository, in `~/rfmap-data`. Any directory works; use the same
path in every command.

```sh
mkdir -p ~/rfmap-data/inputs
docker build -t rfmap/tiles-builder ~/fpv-radio-map/tools/tiles-builder
```

The image build takes a few minutes the first time. Build it again after every `git pull` that
changes `tools/tiles-builder`: fixes to the builder only reach you through a new image.

You need two values:

- **A bbox**, `minLon,minLat,maxLon,maxLat`. Draw one at <https://boundingbox.klokantech.com>
  and pick "CSV". It sets the area of the DEM, land cover and satellite layers, and the basemap
  is cut to it. Areas across the 180° meridian are not supported.
- **A Geofabrik region** for the basemap, as a path from <https://download.geofabrik.de>
  without `-latest.osm.pbf`: `europe/ukraine`, `europe/poland`, `europe/germany/bayern`.
  The basemap has OSM data only from this extract. Where the bbox reaches into a neighbouring
  country, terrain and land cover are there but the basemap is empty. For a border area, use
  the next larger extract (`europe` is very large; see the RAM note below).

Look up the extract's size on its Geofabrik page. It decides the memory the basemap step needs:
give Java 2–3× the `.osm.pbf` size, at least 2 GB, with `--java-heap` **(estimate)**.

Run the builder steps inside `tmux` or `screen`. A country takes hours, and a dropped SSH session
stops the container. Every command below is safe to repeat: finished files are skipped, and
downloads resume from `~/rfmap-data/cache`.

The commands use a small shell function (bash and zsh) so you type the Docker options once:

```sh
tb() { docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder "$@"; }
BBOX=34.8,48.3,35.4,48.7
```

Replace the bbox with yours. Define both again in every new shell. The steps can run one at a
time, as below, or together with `tb all` and the options of all three. `tb all` builds the
basemap only from `--region` or `--osm-file`; with `--osm-url`, run `tb basemap` on its own.

### 2.3 Basemap (OpenStreetMap)

```sh
tb basemap --bbox "$BBOX" --region europe/ukraine --basemap-maxzoom 14 --java-heap 6g
```

- `--basemap-maxzoom 14` gives a file several times smaller that looks the same: MapLibre
  over-zooms it. Leave it out for z16.
- The step also downloads water polygons (osmdata.openstreetmap.de) and Natural Earth
  (naciscdn.org) once, into `cache/`.
- `--basemap-full-extract` keeps the whole extract instead of cutting it to the bbox.
- A Geofabrik mirror or another extract: `--osm-url https://.../file.osm.pbf`, or put the file
  in `~/rfmap-data/inputs/` and pass `--osm-file file.osm.pbf`.
- Java runs out of memory with `java.lang.OutOfMemoryError` or the container is killed (exit
  code 137): raise `--java-heap`, and on Docker Desktop the VM's memory too.

Output: `basemap.pmtiles`.

### 2.4 Terrain: DEM, hillshade and contours

```sh
tb dem --bbox "$BBOX"
```

- The calculation DEM, `dem.cog.tif`, keeps the source heights bit for bit; the build compares
  every pixel with the source and stops if one differs. Hillshade and contours are pictures for
  the map only.
- `--contour-interval 10` draws 10 m contours instead of 20 m. `--no-hillshade` and
  `--no-contours` skip those layers.
- Missing Copernicus tiles (open sea, or areas the provider withholds) are logged as
  `Copernicus tile ... does not exist` and stay NoData. They are never filled with invented
  heights, and the manifest lists them under `missing_tiles`.
- Copernicus not reachable: `--dem-source srtm` uses AWS Terrain Tiles (mostly SRTM). Your own
  DEM: put the files in `inputs/` and pass `--dem-file name.tif` once per file.

**Areas that cross 50° N (or 60°, 70°, 80°).** North of 50° N, Copernicus DEM tiles have a wider
longitude spacing (1.5″ instead of 1″), and the two grids are offset by a fraction of a pixel. In
Ukraine, 50° N runs just south of Kyiv, through Kharkiv and just north of Lviv; in Poland it
runs just south of Kraków, so almost the whole country lies north of it. Without help the build stops:

```
the DEM tiles for this bbox have different pixel sizes ... pass --allow-dem-resample
```

Add `--allow-dem-resample`. The build then logs this warning, which is expected and not an error:

```
WARNING: mixing pixel sizes: coarser tiles are duplicated nearest-neighbour onto the finest grid
```

The northern tiles are put once onto the 1″ grid by duplicating pixels; no height is
interpolated or invented. Tiles already on that grid stay bit for bit, the pixel-by-pixel check
still runs, and the manifest records the duplication. The alternative is to split the area at
50° N, but the server reads one `dem.cog.tif`, so you would run two servers. A builder image from
before 20 October 2026 could fail the pixel check after this warning; build the image again.

Output: `dem.cog.tif`, `hillshade.pmtiles`, `contours.pmtiles`.

### 2.5 Land cover (ESA WorldCover)

```sh
tb landcover --bbox "$BBOX"
```

- The builder reads only the part of each 3°×3° WorldCover tile inside the bbox, over HTTP range
  requests, and keeps it in `cache/` as one file per tile and bbox. A rerun with the same bbox reuses these
  files; a different bbox downloads new windows. Class values are copied unchanged.
- `--worldcover-year 2020` uses the older v100 map; the default is 2021 (v200).
- **Network errors are retried.** Over a large area a single range request to the AWS bucket can
  fail (GDAL reports response code 0, followed by a decode error such as `ZIPDecode`). GDAL
  retries each request up to 10 times, and the builder retries the whole tile window 4 times,
  logging `WorldCover download failed (attempt N of 4); retrying in ... s`. If a tile still fails,
  the step stops; run the same command (same bbox) again and only the missing windows are downloaded. A
  builder image from before 20 October 2026 restarted the whole step instead; build the image
  again.

Output: `landcover.cog.tif`.

### 2.6 Satellite imagery (optional)

Satellite imagery is the only layer without a default source, because each mosaic has its own
licence and terms of use. Without it the map has no **Map / Satellite** switch; everything else
works. The builder supports three kinds of source:

| Source | Option | Notes |
|---|---|---|
| Sentinel-2 cloudless by EOX, years 2016 and 2018–2023 | `--sat-provider eox-s2cloudless-<year>` | Tiles are copied from `tiles.maps.eox.at`. EOX lists 2016 as CC BY 4.0 and 2018–2023 as CC BY-NC-SA 4.0 (non-commercial). |
| A listed mosaic you downloaded yourself (GeoTIFF/COG/VRT) | `--sat-file name.tif --sat-mosaic <mosaic>` | `<mosaic>` is one of the EOX names above, or `copernicus-sentinel-2` for any Sentinel-2 imagery, together with `--sat-data-year <year>`. |
| Any other mosaic: an https XYZ template or a raster | `--sat-source <template or path or URL>`, plus `--sat-name`, `--sat-licence` and `--sat-attribution`. These three are optional, but without the licence and attribution the manifest marks the licence `UNVERIFIED`. | Template example: `https://host/path/{z}/{y}/{x}.jpg` (`{-y}` for TMS rows). |

**Step 1: check the licence.** Read the provider's current licence and terms of service, and
whether they allow downloading tiles in bulk. Many tile services, including most commercial
imagery (Google, Bing, Esri, Mapbox), forbid it; do not use them with `--sat-source`. A
non-commercial licence (EOX 2018–2023) fits a non-commercial site only.

**Step 2: count the tiles.** The XYZ download is one request per tile, rate-limited to
`--sat-rate` requests per second (default 4), and the builder refuses jobs above
`--sat-max-tiles` (default 50,000) with `N tiles needed (> --sat-max-tiles 50000)`. Run the
command once without raising the limit to see N. Some figures for z6 to the given maximum zoom:

| Area | `--sat-maxzoom 13` | `--sat-maxzoom 14` (default) |
|---|---|---|
| District, 0.6°×0.4° (the example bbox) | 331 tiles, under 2 min | 1,143 tiles, about 5 min |
| Poland, `14.07,49.0,24.15,54.84` | 66,735 tiles, about 5 h | 265,915 tiles, about 18 h |

Sentinel-2 has 10 m pixels, so z14 (about 6 m per pixel at 50° N) shows all its detail, and z13
is only slightly softer. Disk: about 100–200 MB per 1°×1° at z14 **(estimate)**. Raise
`--sat-max-tiles` only to what the provider's terms allow, and keep `--sat-rate` at its default
or lower.

**Step 3: build.**

```sh
tb satellite --bbox "$BBOX" --sat-provider eox-s2cloudless-2016 --sat-licence-checked
```

`--sat-licence-checked` records that you did step 1. Passing both `--sat-licence` and
`--sat-attribution` records it too. With neither, the manifest marks the licence `UNVERIFIED`. An
interrupted download resumes from `cache/satellite-xyz/`. The step prints
`XYZ: fetched ..., already cached ..., not available ...`; tiles the provider does not have
stay empty.

A large country at z14, with a higher limit after checking the terms:

```sh
tb satellite --bbox 14.07,49.0,24.15,54.84 --sat-provider eox-s2cloudless-2016 \
    --sat-licence-checked --sat-max-tiles 300000
```

Output: `satellite.pmtiles`. The server reads `manifest.json` each time a browser asks for the map
settings, so the switch appears on the next page load, without a restart.

### 2.7 Check the result

```sh
tb verify
ls ~/rfmap-data/out
```

`verify` checks the `.tif` and `.pmtiles` files that exist, and each must be reported `OK`. It
does not notice a missing file, so also check the listing: you should see `dem.cog.tif`, `landcover.cog.tif`,
`basemap.pmtiles`, `hillshade.pmtiles`, `contours.pmtiles`, `style-dark.json`, `manifest.json`
and `fonts/`, plus `satellite.pmtiles` if you built it. `manifest.json` lists each file with its
source, licence, attribution, bbox and resolution; check that the licence of your satellite
mosaic is not `UNVERIFIED`.

`~/rfmap-data/out` is the directory the server will use. `cache/` and `work/` next to it are
only for the builder; keep `cache/` while you may rebuild, and delete both when you need the disk
space (never while a build runs).

### 2.8 How the layers reach the map

You do not edit compose or nginx for the layers. In step 3 you set one variable in `.env`:

```
RFMAP_TILES_DIR=/home/<you>/rfmap-data/out
```

Compose mounts that directory into `rfmap-server`, which calculates with the `.tif` files, and
into `web`, whose nginx serves the `.pmtiles` files at `/tiles/` to browsers. The browser asks
the server which layers exist (`GET /api/v1/config/map`); the server lists only the layers in
`manifest.json` (all four if there is no readable manifest), then applies the `map` object of
its configuration, if any. [How map layers reach the browser](map-layers.md) has the details, diagrams,
the settings you can change (another tiles host, hiding a layer) and checks for a layer that
does not show.

After you rebuild a layer:

| You rebuilt | Then |
|---|---|
| `basemap`, hillshade, contours or `satellite`, replacing a layer that already existed | nothing. nginx lets browsers cache `/tiles/` for an hour, so open browsers show the new files within an hour, or at once after a hard reload |
| a layer that did not exist before (for example the first `satellite`), or one you removed | no restart; open browsers need a page reload, because the web app reads the list of layers once when the page loads |
| `dem` or `landcover` | `cd ~/fpv-radio-map/deploy && docker compose restart rfmap-server`, so the server opens the new files |

### 2.9 Example: Poland with satellite imagery

Poland crosses 50° N, its `.osm.pbf` is about 2 GB, and the bbox below is about 10°×6°. Give
the machine 16 GB RAM and 60 GB of free disk **(estimate)**:

```sh
tb() { docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder "$@"; }
tb all --bbox 14.07,49.0,24.15,54.84 --region europe/poland \
    --basemap-maxzoom 14 --java-heap 12g --allow-dem-resample
tb satellite --bbox 14.07,49.0,24.15,54.84 --sat-provider eox-s2cloudless-2016 \
    --sat-licence-checked --sat-max-tiles 300000
tb verify
```

Expect the `mixing pixel sizes` warning in the DEM step and, possibly, a few `WorldCover download
failed ... retrying` lines in the land cover step. If any step stops on a network error, run the
same command again. The bbox includes parts of the neighbouring countries: they get terrain, land
cover and satellite imagery, but no basemap.

## 3. Secrets

```sh
cd ~/fpv-radio-map/deploy
cp .env.example .env
chmod 600 .env
sed -i \
    -e "s|^POSTGRES_PASSWORD=.*|POSTGRES_PASSWORD=$(openssl rand -hex 24)|" \
    -e "s|^RFMAP_ADMIN_TOKEN=.*|RFMAP_ADMIN_TOKEN=$(openssl rand -base64 33)|" \
    -e "s|^RFMAP_TILES_DIR=.*|RFMAP_TILES_DIR=$HOME/rfmap-data/out|" \
    .env
```

On macOS, set the same three values with a text editor. Keep the database password to letters
and digits (hex, as above): it becomes part of a database URL, where `/`, `@` or `:` would break
it.

Then open `.env` and fill in the DEM error, if you have it:

| Variable | Value |
|---|---|
| `RFMAP_DEM_STATED_ERROR_M` | The vertical error your DEM provider states, in metres. For Copernicus DEM GLO-30 it is in the Copernicus DEM Product Handbook (absolute vertical accuracy). |
| `RFMAP_DEM_STATED_ERROR_SOURCE` | Where you took that figure from: document name, version, section. |
| `RFMAP_HTTP_PORT` | Leave `8088` for now. Step 8 changes it to `127.0.0.1:8088`. |

Without a stated error the server works, logs
`data.demStatedErrorM is not set: results carry no terrain error`, and every result says so. Do
not guess the figure.

The admin token is in `.env`. You need it for the Admin pages; show it with
`grep RFMAP_ADMIN_TOKEN .env`.

## 4. Owner key

Modules signed with the owner key run inside the server process. The server image contains the
`rfmod` tool that makes the key, so build the image once without a key, make the key, and build
again with it.

```sh
cd ~/fpv-radio-map/deploy
docker compose build rfmap-server
mkdir -p ~/rfmap-keys && chmod 700 ~/rfmap-keys
docker run --rm --user "$(id -u):$(id -g)" --entrypoint /usr/bin/rfmod \
    -v "$HOME/rfmap-keys:/keys" rfmap-server:local keygen --out /keys/owner
sed -i "s|^RFMAP_OWNER_PUBKEY=.*|RFMAP_OWNER_PUBKEY=$(cat ~/rfmap-keys/owner.pub)|" .env
grep RFMAP_OWNER_PUBKEY .env   # RFMAP_OWNER_PUBKEY=rfmod-ed25519-pub:...
```

`owner.secret` signs modules for this server. Anyone who has it can run code inside your server.
You need it once more in step 6; afterwards copy `~/rfmap-keys` to offline storage (an
encrypted USB stick, a password manager) and delete it from the server:

```sh
shred -u ~/rfmap-keys/owner.secret
```

## 5. Start the stack

Run the first start inside `tmux` or `screen`: `docker compose up -d` does not return until
`open-meteo-init` has downloaded the first copy of the forecast model (about 6 GB, see
[Weather](#9-weather)), and a dropped SSH session would cut that short.

```sh
cd ~/fpv-radio-map/deploy
docker compose up -d --build
docker compose ps
```

The first build takes a few minutes **(estimate)**. `postgis`, `rfmap-server` and `web` should be
`healthy` or `running` after about a minute, while `up` is still waiting for `open-meteo-init`;
check them from a second shell. `open-meteo` and `open-meteo-sync` start once `open-meteo-init`
has completed successfully, and run without a health check.

```sh
curl -s http://127.0.0.1:8088/api/v1/health
# {"dem":true,"landcover":true,"ok":true,"version":"0.1.0","weather":true}
docker compose logs rfmap-server | head -20
```

`dem` and `landcover` must be `true`. In the log you should see `applied migration 001_init.sql`,
`seeded 4 radio-profiles` (DJI video, `seed-video.json`), `seeded 14 radio-profiles`,
`seeded 38 antennas`, `seeded 21 batteries`, `seeded 3 drones`,
`terrain: DEM /data/dem.cog.tif` and `listening on 0.0.0.0:8080`. No `seeded` line may end in
`skipped`: a `seed ... rejected` line before it names the entry and the reason. If
the log says `built with the PUBLIC development owner key`, the key from step 4 did not reach the
build: check `.env` and run `docker compose up -d --build` again.

Open <http://127.0.0.1:8088> in a browser on the server (or `http://<server-ip>:8088` from your
network). The map should show your area with hillshade and the attribution line at the bottom.

## 6. Install the FPV modules

A new server has only the built-in modules (`rf.weather`, `rf.clutter`, `rf.uncertainty`,
`rf.radio-profiles`, `rf.flightlog`). Battery, minimum altitude, failsafe, best launch point, relay,
the Blackbox flight log decoder and the mission tools come as the FPV module pack. Sign it with
your owner key and install it on the server machine with the `rfmod` CLI: modules never enter a
server through the API.

Build and sign the seven modules (about a minute **(estimate)**; the compile reuses the server
image's build cache):

```sh
cd ~/fpv-radio-map
docker build -f deploy/server.Dockerfile --target build \
    --build-arg RFMAP_OWNER_PUBKEY="$(cat ~/rfmap-keys/owner.pub)" -t rfmap-build .
mkdir -p ~/rfmap-modules
docker run --rm -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$HOME/rfmap-keys/owner.secret:/owner.secret:ro" -v "$HOME/rfmap-modules:/out" \
    rfmap-build sh -c '
      set -e
      cmake --build build >/dev/null
      for s in build/modules/fpv/*/stage; do
        build/platform/rfmod/rfmod pack "$s" --key /owner.secret -o "/out/$(basename "$(dirname "$s")").rfmod"
      done
      chown -R "$HOST_UID:$HOST_GID" /out'
```

Install them, dependencies first (`failsafe` needs `battery` and `min-altitude`). The packages
go into the server's state volume, the CLI inside the server container installs them, and one
API call makes the running server pick them up:

```sh
cd ~/fpv-radio-map/deploy
docker compose cp "$HOME/rfmap-modules/." rfmap-server:/var/lib/rfmap/incoming/
for m in core battery min-altitude failsafe best-launch relay blackbox; do
  docker compose exec rfmap-server rfmod install "/var/lib/rfmap/incoming/$m.rfmod" \
      --root /var/lib/rfmap/modules --trust-store /var/lib/rfmap/keys.json
done
docker compose exec rfmap-server rm -r /var/lib/rfmap/incoming
TOKEN=$(sed -n 's/^RFMAP_ADMIN_TOKEN=//p' .env)
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8088/api/v1/admin/modules/reload
```

Every `rfmod install` must end with `installed`. The CLI checks the signature, manifest, ABI,
dependencies and lifecycle and runs each module's self-test in a sandboxed worker before it
installs it. On a failure the report names the check with `ok: false`. A version that is
already installed is refused (`version.upgrade`); that is expected when you run the loop twice.
The browser's **Admin → Modules** page shows the result after **Reload**; it has no upload.

Check the list: `curl -s http://127.0.0.1:8088/api/v1/modules` shows the five `rf.*` modules and
the seven `fpv.*` ones. Now delete `owner.secret` from the server, as described in step 4.

## 7. Fill the equipment library

Pilots pick radios, antennas, batteries and drones from the library, and only the admin edits
it. A new server has 14 control-link radio profiles (ExpressLRS and Crossfire), 4 DJI video
profiles, 38 antennas, 21 batteries and 3 DJI drone profiles, all `verified: false`. The README
next to each seed file says where each figure comes from:
[radio profiles](../../data/library/radio-profiles/README.md),
[antennas](../../data/library/antennas/README.md),
[batteries](../../data/library/batteries/README.md) and
[drones](../../data/library/drones/README.md). Before the first flight plan:

1. Open **Admin** in the web app, enter the admin token at the top right and choose **Use token**.
   The token stays in that browser tab only.
2. Under **Library**, review the seeded entries against their sources and mark the ones you
   checked as verified. The drone profiles are derived from DJI's flight-time figures, not
   measured; replace them with your own flight logs when you have them.
3. Add the antennas, batteries and drones you fly that are not seeded. Every link needs a
   ground antenna and an air antenna.
4. Add your video system as a radio profile if it is not one of the seeded DJI links
   (Walksnail, HDZero and analog video are not seeded; see the radio profiles README).

Entries are edited as JSON and checked against the library schema as you type. A plain
half-wave dipole, for example:

```json
{
  "id": "dipole-ground", "name": "Half-wave dipole (ground)", "version": 1, "verified": true,
  "type": "dipole", "gainDbi": 2.15, "polarization": "linear-vertical",
  "bandHz": [4e8, 6e9], "feederLossDb": 0.5,
  "source": { "kind": "standard", "reference": "half-wave dipole, 2.15 dBi" }
}
```

Every entry needs a source (datasheet, standard, measurement or derived). Edits are versioned, and
results name the versions they used. Scripted imports use `POST /api/v1/library/<kind>` with
the admin token; see the [API reference](../api/openapi.yaml).

## 8. TLS

Share links, PINs and the admin token must not travel over plain HTTP. Put your TLS reverse
proxy (or load balancer) in front of `web`, and let only that proxy reach it.

Bind `web` to localhost. In `deploy/.env`:

```
RFMAP_HTTP_PORT=127.0.0.1:8088
```

```sh
cd ~/fpv-radio-map/deploy && docker compose up -d
```

Run the proxy on the host (a system package), not in a container behind published ports:
there Docker hides the address of every IPv6 visitor, and under rootless Docker of every
visitor, so they would all share one rate limit. Point it at `http://127.0.0.1:8088` for your
domain. It must:

- forward every path (`/`, `/api/`, `/tiles/`) and keep the `Range` header;
- not compress `.pmtiles` responses;
- add the visitor's address to `X-Forwarded-For` (nginx:
  `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;`).

**Tell `web` the proxy's address,** or rfmap-server sees every visitor with the proxy's address
and all visitors share the per-address rate limits. A proxy on this machine reaches `web` from
the Docker network's gateway, in `172.16.0.0/12`. Create `deploy/nginx-realip.conf`:

```nginx
# Your reverse proxy, as nginx sees it. nginx takes the client's address from the
# X-Forwarded-For header only on connections from these addresses.
set_real_ip_from 172.16.0.0/12;
real_ip_header X-Forwarded-For;
```

and `deploy/docker-compose.override.yml`, which Compose reads next to `docker-compose.yml`:

```yaml
services:
  web:
    volumes:
      - ./nginx-realip.conf:/etc/nginx/conf.d/realip.conf:ro
```

```sh
cd ~/fpv-radio-map/deploy && docker compose up -d
```

Check it: open the site through the proxy, then run `docker compose logs web | tail -3`. Each
line must start with your own address, not `172.x.x.x`. Keep `RFMAP_HTTP_PORT` on `127.0.0.1`
with this file in place: on a public port, visitors that Docker forwards from the gateway
could set their own address. A proxy on another machine, and the
address range to use when the gateway is not in `172.16.0.0/12`, are covered under
[Behind a reverse proxy](deploy.md#behind-a-reverse-proxy).

## 9. Weather

`open-meteo-init` downloads the forecast model once (about 6 GB; follow it with
`docker compose logs -f open-meteo-init`), then the `open-meteo` API and `open-meteo-sync`, which
keeps the data fresh, start. Until then, results note that the forecast is unavailable and use
k = 4/3. If the server reboots before that first download has finished, run
`docker compose up -d` again. Check the instance once the API is up:

```sh
docker run --rm --network rfmap_backend curlimages/curl -s \
  "http://open-meteo:8080/v1/forecast?latitude=48.5&longitude=35.2&hourly=temperature_850hPa,geopotential_height_850hPa,surface_pressure,wind_speed_10m&models=dwd_icon"
```

Every variable should come back with numbers, not `null`. Use your area's coordinates. See
[Open-Meteo](deploy.md#4-open-meteo) for the variables the server asks for.

## 10. Backups

Two things cannot be rebuilt: the database and the `rfmap-state` volume (installed modules and
trusted keys). The server writes both into one file. It reads one consistent snapshot, so the
stack keeps running while it works. An `rfmod install` or `rfmod remove`, a **Reload** and a
module setting saved meanwhile wait until the backup has read the files:

```sh
cd ~/fpv-radio-map/deploy
mkdir -p ~/rfmap-backup && chmod 700 ~/rfmap-backup
docker compose exec -T rfmap-server rfmap-server --backup > ~/rfmap-backup/rfmap-$(date +%F).backup.zst
```

It prints what it wrote, for example `backed up 9 tables, 120 rows and 3 files`. The file is
compressed with zstd: `zstdcat` shows it as text, one JSON object per line. The first line names
the schema it was written under (its last migration, how many there are and a fingerprint of
them), and the last line starts with `{"end":true`. The file holds data only: the rows of each
table, the module files and the trusted keys, never the schema. The result cache is left out: it
fills again. A restore reads an uncompressed backup of an older version too. Keep the owner key offline (step 4), and copy `~/rfmap-backup` and `deploy/.env`
to another machine. The map data can always be built again from step 2.

### 10.1 Restore on a new server

A restore builds the schema with the server's own migrations, loads the rows and applies the
migrations that came after the backup, all in one transaction, then puts the module files back
once that transaction has committed. A backup therefore restores into the same or a newer
server, on any PostgreSQL version, and the migrations adapt the data to the new version. It
restores only into a database that has no schema yet. If it stops, nothing is committed to the database, which stays empty, and no module
file or trust store (`keys.json`) is changed: fix the cause and run it again. The files go into
place only after the database has committed, each replacing the file of the same name.

One failure comes after that commit: a file that cannot move into place, for example because a
directory of the same name is in the way. The rows are then restored, and the error names the
file. Do not run the restore again, since the database now has a schema. Clear what is in the
way, move each file that
ends in `.rfmap-restoring` to the same name without that ending, in the `rfmap-state` volume,
and start the stack:

```sh
docker compose run --rm --no-deps --entrypoint sh rfmap-server -c \
  'find /var/lib/rfmap -name "*.rfmap-restoring" -exec sh -c "mv \"\$1\" \"\${1%.rfmap-restoring}\"" _ {} \;'
docker compose up -d
```

1. **On the old server**, stop the server so nothing changes after the snapshot, and back up:

   ```sh
   cd ~/fpv-radio-map/deploy
   docker compose stop rfmap-server
   docker compose run --rm -T --no-deps rfmap-server --backup > ~/rfmap-backup/rfmap-$(date +%F).backup.zst
   ```

   Copy the backup and `deploy/.env` to the new server.

2. **On the new server**, do steps 0–2 as for a first install (Docker, code, map data). Skip
   steps 3 and 4: put the old `.env` in `deploy/` instead. It holds the database password, the
   admin token and the owner public key the backup belongs to; correct `RFMAP_TILES_DIR` if the
   map data lives in another path here. Then build the server image and start only the
   database, which comes up empty:

   ```sh
   cd ~/fpv-radio-map/deploy
   docker compose build rfmap-server
   docker compose up -d --wait postgis
   ```

3. Restore (`<date>` is the date in the backup's name):

   ```sh
   docker compose run --rm -T --no-deps rfmap-server --restore < ~/rfmap-backup/rfmap-<date>.backup.zst
   ```

   It prints each migration it applied and `restored 9 tables, 120 rows and 3 files`.

4. Start the stack (step 5), then do step 8 (TLS). Open a mission you know and check that it is
   there. The modules came back with the backup, so step 6 is not needed.

### 10.2 Move to a new PostgreSQL major version

A new PostgreSQL major version cannot open the data folder of the previous one, so the pull that
moves `postgis` to it (16 to 18 is one) moves the data with a backup and a restore on the same
server. The new version keeps its data in a new volume (`rfmap_pgdata18`), and the old one
(`rfmap_pgdata`) is not touched:

```sh
cd ~/fpv-radio-map && git pull && cd deploy &&
  docker compose build rfmap-server &&
  mkdir -p ~/rfmap-backup && chmod 700 ~/rfmap-backup &&
  backup=~/rfmap-backup/rfmap-$(date +%F).backup.zst &&
  docker compose stop rfmap-server &&
  docker compose run --rm -T --no-deps rfmap-server --backup > "$backup" &&
  docker compose up -d --wait postgis &&
  docker compose run --rm -T --no-deps rfmap-server --restore < "$backup" &&
  docker compose up -d --build
```

Each step runs only when the one before it succeeded, so a failed backup never goes on to the
new database. If a step before `up -d --wait postgis` fails, fix the cause and run the block
again. If the restore fails, the new database stays empty: fix the cause and run only the last
two commands again, with the same `backup=` path. Do not run the backup again at that point, since
it would now read the new, empty database.

The backup runs before `up` replaces the old `postgis` container, so the new server reads the old
database. `rfmap-server` is down from `stop` to the last `up`: the web container keeps running,
but its `/api` requests cannot reach the server. On a small database that is under a minute. To go back, check out the previous version and run `docker compose up -d`: it opens the old
volume again, without anything written after the move. Remove the old volume
(`docker volume rm rfmap_pgdata`) once the new database has run for a while and you have a backup
of it. Never use `docker compose down -v`: it removes the volumes.

## 11. Updates

**The application:**

```sh
cd ~/fpv-radio-map
git pull
cd deploy && docker compose up -d --build
```

Migrations run at start. A pull that moves the database to a new PostgreSQL major version (16 to
18 is one) moves the data with a backup and a restore: see
[Move to a new PostgreSQL major version](#102-move-to-a-new-postgresql-major-version).

The server refuses to start, and says why in `docker compose logs rfmap-server`, in three cases:

- **A breaking migration.** It changes existing data so that the previous version can no longer
  read it, so the server waits for you to take a backup first. See the commands below.
- **A database newer than the server.** You checked out an older version after a newer one had
  migrated the database. Go forward again, or restore the backup you took before that update
  into a new, empty database with the older version (§10.1).
- **A migration that was changed after it was applied.** That is a bug in the release; report it
  and stay on the previous version.

To apply a breaking migration, back up with the running version, then migrate with the new one:

```sh
cd ~/fpv-radio-map/deploy &&
  mkdir -p ~/rfmap-backup && chmod 700 ~/rfmap-backup &&
  backup=~/rfmap-backup/rfmap-$(date +%F).backup.zst &&
  docker compose stop rfmap-server &&
  docker compose run --rm -T --no-deps rfmap-server --backup > "$backup" &&
  cd .. && git pull && cd deploy &&
  docker compose build rfmap-server &&
  docker compose run --rm -T --no-deps rfmap-server --migrate --plan &&
  docker compose run --rm -T --no-deps rfmap-server --migrate &&
  docker compose up -d --build
```

`--migrate --plan` lists the migrations it would apply and changes nothing. `--migrate` applies
them all in one transaction: if it fails, the database stays as it was. To go back after
`--migrate`, check out the previous version and restore `$backup` into a new, empty database
(§10.1). When a
module's version in `modules/fpv/*/manifest.json` goes up, repeat step 6 with your offline owner key; the server takes only versions newer than the
installed one.

**New OSM data** (monthly is plenty), with the same `--bbox` and options as in step 2:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder basemap \
    --bbox 34.8,48.3,35.4,48.7 --region europe/ukraine --basemap-maxzoom 14 --java-heap 6g --refresh-osm
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder verify
```

Browsers pick up the new basemap within an hour (see [How the layers reach the map](#28-how-the-layers-reach-the-map)).

**A different area:** run the `all` command from step 2 with the new `--bbox` (and `satellite`, if
you use it), run `verify`, then restart the server so it opens the new DEM and land cover files:

```sh
cd ~/fpv-radio-map/deploy && docker compose restart rfmap-server
```

After a `git pull` that changes `tools/tiles-builder`, build its image again (step 2).

## When something fails

| Symptom | Cause and fix |
|---|---|
| `permission denied` on `/var/run/docker.sock` | Your user is not in the `docker` group yet. Log out and in after `usermod`. |
| tiles-builder: `Permission denied: '/data/out'` | `--user "$(id -u):$(id -g)"` is missing, or `~/rfmap-data` belongs to another user (`sudo chown -R "$USER" ~/rfmap-data`). |
| tiles-builder: `different pixel sizes` | The DEM tiles have different pixel sizes: the bbox crosses 50° N (or 60°, 70°, 80°), or the `--dem-file` inputs use different grids. Add `--allow-dem-resample`, see [step 2.4](#24-terrain-dem-hillshade-and-contours). |
| tiles-builder: `WARNING: mixing pixel sizes` | Expected with `--allow-dem-resample`; not an error. |
| tiles-builder: DEM pixel check fails after `mixing pixel sizes` | The builder image is older than the fix. `git pull` and build the image again (step 2.2); downloads are reused. |
| tiles-builder: `WorldCover download failed (attempt N of 4)` | A network error on the WorldCover bucket; it is retried. If the step stops, run it again: only missing tiles are downloaded. An old image restarts the whole step, so build it again. |
| tiles-builder: `java.lang.OutOfMemoryError`, or the basemap step exits with code 137 | Not enough memory for the OSM extract. Raise `--java-heap` (2–3× the `.osm.pbf` size) and the RAM of the machine or Docker Desktop. |
| tiles-builder: `tiles needed (> --sat-max-tiles ...)` | The satellite area is larger than the safety limit. Lower `--sat-maxzoom`, or raise `--sat-max-tiles` after checking the provider's terms, see [step 2.6](#26-satellite-imagery-optional). |
| tiles-builder: satellite `HTTP 403` (or `401`) | The tile service refuses access, often because its terms forbid bulk downloads. Check the provider's access and licence terms, or choose another mosaic; a slower rate does not help. |
| tiles-builder: satellite `HTTP 429` | The tile service throttles you. Each tile is retried a few times; if the step still stops, lower `--sat-rate` and run it again. Downloaded tiles are kept. |
| No **Map / Satellite** switch | A readable `manifest.json` does not list `satellite.pmtiles`: build the `satellite` step and run `verify`. Also check that the `map` object of the server configuration does not remove `satellite`. |
| tiles-builder: `cannot read the Geofabrik index` | Geofabrik is blocked on your network. Download the `.osm.pbf` elsewhere into `~/rfmap-data/inputs/` and pass `--osm-file <name>` instead of `--region`. The same works for the other sources: see [known limitations](tiles-builder.md#10-known-limitations-and-honest-notes). |
| `docker compose`: `set POSTGRES_PASSWORD in .env` | `.env` is missing or not in `deploy/`. Run compose from `deploy/`. |
| Image build: `429 Too Many Requests` from Docker Hub | Docker Hub rate limit. Wait, or `docker login` with a free account. |
| Image build: `certificate verify failed` | A TLS-intercepting proxy. Pass its CA as a build secret, see [Start](deploy.md#2-start). |
| `health` shows `"dem":false` | `RFMAP_TILES_DIR` does not point at the directory that holds `dem.cog.tif`, or `data.dem` (`RFMAP_DEM`) in the server configuration names another path. Write the full path (`/home/<you>/rfmap-data/out`, not `~/...`) and run `docker compose up -d`. |
| Map is empty, `/tiles/...` answers 404 | `RFMAP_TILES_DIR` is wrong for the `web` container, or the file was not built: `docker compose exec web ls /tiles` must list it. |
| `rfmod install`: `signature` check fails | The module was signed with a different key than the one the server was built with. Sign it with your `owner.secret` (step 6). |
| The server exits at start with `development owner key` | The image was built without `RFMAP_OWNER_PUBKEY`. Build it with your `owner.pub` (step 4); `modules.allowDevOwnerKey` is for development machines only. |
