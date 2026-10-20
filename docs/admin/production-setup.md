# Production server, step by step

This guide takes one Linux machine from nothing to a running FPV Radio Map server with your own
map data. It puts the steps from [Building map data](tiles-builder.md) and
[Deployment](deploy.md) in the order you run them, with the commands to copy. Those two pages
have every option and the reasons behind them.

You will:

1. build the map data (tiles) for your area;
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
| Network | Outbound HTTPS while you build: Docker Hub, GitHub and its container registry, Ubuntu and npm mirrors, and the map data sources (Geofabrik, AWS Open Data, osmdata.openstreetmap.de, naciscdn.org). At run time only the Open-Meteo sync service goes out. |
| Public server | A domain name pointing at the machine, and ports 80 and 443 open. |

Install Docker from <https://docs.docker.com/engine/install/>, then let your user run it and log
in again:

```sh
sudo usermod -aG docker "$USER"
```

Check both parts:

```sh
docker info --format '{{.ServerVersion}}'   # prints a version, not an error
docker compose version                      # Docker Compose version v2.x or later
```

Docker Desktop on Windows or macOS is fine for trying it out (Settings → Resources: 8 GB RAM or
more). Run a public server on Linux.

## 1. Get the code

```sh
cd ~
git clone https://github.com/Rufics/fpv-radio-map.git
```

## 2. Build the map data

The map data lives outside the repository, in `~/rfmap-data`. Any directory works; use the same
path in every command.

```sh
mkdir -p ~/rfmap-data
docker build -t rfmap/tiles-builder ~/fpv-radio-map/tools/tiles-builder
```

The image build takes a few minutes the first time.

**Choose the area.** You need a bbox (`minLon,minLat,maxLon,maxLat`, draw one at
<https://boundingbox.klokantech.com> and pick "CSV") and a Geofabrik region for the basemap
(`europe/ukraine`, `monaco`, ... see <https://download.geofabrik.de>). Before you choose:

- The bbox decides the DEM, land cover and satellite area. The basemap is cut to the same bbox.
- **50° N:** Copernicus DEM changes its pixel spacing at 50° latitude, and the build stops when
  the bbox crosses it. In Ukraine, 50° N runs just south of Kyiv, through Kharkiv and just north
  of Lviv. The server reads one
  DEM file, so for an area that crosses 50° N add `--allow-dem-resample`. It duplicates the
  coarser pixels without inventing heights, and the manifest records it.
- The basemap step reads the whole Geofabrik extract, even when it keeps only your bbox. Check
  the extract's size on its Geofabrik page and give Java 2–3× that, at least 2 GB, with
  `--java-heap` **(estimate)**.

**Build everything except satellite imagery:**

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder all \
    --bbox 34.8,48.3,35.4,48.7 \
    --region europe/ukraine \
    --basemap-maxzoom 14 \
    --java-heap 6g
```

Replace the bbox and region with yours. `--basemap-maxzoom 14` keeps the basemap several times
smaller with the same look; leave it out for z16. Running the same command again is safe:
finished files are skipped, and interrupted downloads resume.

**Satellite imagery is optional,** and you must check its licence first. See
[Satellite](tiles-builder.md#4-run) for the command and the licence notes. Without it, the map
has no satellite view; everything else works.

**Check the result:**

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder verify
ls ~/rfmap-data/out
```

Every file must be reported `OK`. You should see `dem.cog.tif`, `landcover.cog.tif`, `basemap.pmtiles`,
`hillshade.pmtiles`, `contours.pmtiles`, `style-dark.json`, `manifest.json` and `fonts/`.
`~/rfmap-data/out` is the directory the server will use. `cache/` and `work/` next to it are
only for the builder.

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

```sh
cd ~/fpv-radio-map/deploy
docker compose up -d --build
docker compose ps
```

The first build takes a few minutes **(estimate)**. `postgis`, `rfmap-server` and `web` should be
`healthy` or `running` after about a minute; `open-meteo` and `open-meteo-sync` run without a
health check.

```sh
curl -s http://127.0.0.1:8088/api/v1/health
# {"dem":true,"landcover":true,"ok":true,"version":"0.1.0","weather":true}
docker compose logs rfmap-server | head -20
```

`dem` and `landcover` must be `true`. In the log you should see `applied migration 001_init.sql`,
`seeded 14 radio-profiles`, `terrain: DEM /data/dem.cog.tif` and `listening on 0.0.0.0:8080`. If
the log says `built with the PUBLIC development owner key`, the key from step 4 did not reach the
build: check `.env` and run `docker compose up -d --build` again.

Open <http://127.0.0.1:8088> in a browser on the server (or `http://<server-ip>:8088` from your
network). The map should show your area with hillshade and the attribution line at the bottom.

## 6. Install the FPV modules

A new server has only the built-in modules (`rf.weather`, `rf.clutter`, `rf.uncertainty`,
`rf.radio-profiles`). Battery, minimum altitude, failsafe, best launch point, relay and the
mission tools come as the FPV module pack. Sign it with your owner key and upload it.

Build and sign the six modules (about a minute **(estimate)**; the compile reuses the server
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

Upload them, dependencies first (`failsafe` needs `battery` and `min-altitude`):

```sh
cd ~/fpv-radio-map/deploy
TOKEN=$(sed -n 's/^RFMAP_ADMIN_TOKEN=//p' .env)
for m in core battery min-altitude failsafe best-launch relay; do
  report=$(curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
      --data-binary @"$HOME/rfmap-modules/$m.rfmod" \
      'http://127.0.0.1:8088/api/v1/admin/modules?install=true')
  case "$report" in
    *'],"ok":true}') echo "$m: installed" ;;
    *) echo "$m: NOT installed"; echo "$report" ;;
  esac
done
```

Every line must say `installed`. The server checks the signature, manifest, ABI, dependencies
and sandbox and runs each module's self-test before it installs it. On a failure the report
names the check with `"ok":false`. A version that is already installed is refused
(`version.upgrade`); that is expected when you run the loop twice. You can also upload the
files one by one under **Admin → Modules** in the browser.

Check the list: `curl -s http://127.0.0.1:8088/api/v1/modules` shows the four `rf.*` modules and
the six `fpv.*` ones. Now delete `owner.secret` from the server, as described in step 4.

## 7. Fill the equipment library

Pilots pick radios, antennas, batteries and drones from the library, and only the admin edits
it. A new server has 14 control-link radio profiles (ExpressLRS and Crossfire), 38 antennas,
21 batteries and 3 DJI drone profiles, all `verified: false` (see `data/library/<kind>/README.md`
for where each figure comes from). Before the first flight plan:

1. Open **Admin** in the web app, enter the admin token at the top right and choose **Use token**.
   The token stays in that browser tab only.
2. Under **Library**, review the seeded entries against their sources and mark the ones you
   checked as verified. The drone profiles are derived from DJI's flight-time figures, not
   measured; replace them with your own flight logs when you have them.
3. Add the antennas, batteries and drones you fly that are not seeded. Every link needs a
   ground antenna and an air antenna.
4. Add your video system as a radio profile if you plan video links.

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

`open-meteo-sync` downloads the forecast model in the background, and the first run takes a
while. Until then, results say that no forecast covers the time and use k = 4/3. Check the
instance once the first run is in:

```sh
docker run --rm --network rfmap_backend curlimages/curl -s \
  "http://open-meteo:8080/v1/forecast?latitude=48.5&longitude=35.2&hourly=temperature_850hPa,geopotential_height_850hPa,surface_pressure&models=dwd_icon"
```

Every variable should come back with numbers, not `null`. Use your area's coordinates. See
[Open-Meteo](deploy.md#4-open-meteo) for the variables the server asks for.

## 10. Backups

Two things cannot be rebuilt: the database and the `rfmap-state` volume (installed modules and
trusted keys). Back them up regularly, and keep the owner key offline (step 4).

```sh
cd ~/fpv-radio-map/deploy
mkdir -p ~/rfmap-backup
docker compose exec -T postgis pg_dump -U rfmap -Fc rfmap > ~/rfmap-backup/rfmap-$(date +%F).dump
docker run --rm -v rfmap_rfmap-state:/state:ro -v "$HOME/rfmap-backup:/backup" alpine:3.20 \
    tar -czf "/backup/rfmap-state-$(date +%F).tgz" -C /state .
```

Copy `~/rfmap-backup` and `deploy/.env` to another machine. The map data can always be built
again from step 2.

## 11. Updates

**The application:**

```sh
cd ~/fpv-radio-map
git pull
cd deploy && docker compose up -d --build
```

Migrations run at start. When a module's version in `modules/fpv/*/manifest.json` goes up,
repeat step 6 with your offline owner key; the server takes only versions newer than the
installed one.

**New OSM data** (monthly is plenty), with the same `--bbox` and options as in step 2:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder basemap \
    --bbox 34.8,48.3,35.4,48.7 --region europe/ukraine --basemap-maxzoom 14 --java-heap 6g --refresh-osm
docker run --rm --user "$(id -u):$(id -g)" -v "$HOME/rfmap-data:/data" rfmap/tiles-builder verify
```

Browsers pick up the new basemap on their next load.

**A different area:** run the `all` command from step 2 with the new `--bbox`, run `verify`, then
restart the server so it opens the new DEM and land cover files:

```sh
cd ~/fpv-radio-map/deploy && docker compose restart rfmap-server
```

After a `git pull` that changes `tools/tiles-builder`, build its image again (step 2).

## When something fails

| Symptom | Cause and fix |
|---|---|
| `permission denied` on `/var/run/docker.sock` | Your user is not in the `docker` group yet. Log out and in after `usermod`. |
| tiles-builder: `Permission denied: '/data/out'` | `--user "$(id -u):$(id -g)"` is missing, or `~/rfmap-data` belongs to another user (`sudo chown -R "$USER" ~/rfmap-data`). |
| tiles-builder: `different pixel sizes` | The bbox crosses 50° N. See step 2. |
| tiles-builder: `cannot read the Geofabrik index` | Geofabrik is blocked on your network. Download the `.osm.pbf` elsewhere into `~/rfmap-data/inputs/` and pass `--osm-file <name>` instead of `--region`. The same works for the other sources: see [known limitations](tiles-builder.md#10-known-limitations-and-honest-notes). |
| `docker compose`: `set POSTGRES_PASSWORD in .env` | `.env` is missing or not in `deploy/`. Run compose from `deploy/`. |
| Image build: `429 Too Many Requests` from Docker Hub | Docker Hub rate limit. Wait, or `docker login` with a free account. |
| Image build: `certificate verify failed` | A TLS-intercepting proxy. Pass its CA as a build secret, see [Start](deploy.md#2-start). |
| `health` shows `"dem":false` | `RFMAP_TILES_DIR` does not point at the directory that holds `dem.cog.tif`. Write the full path (`/home/<you>/rfmap-data/out`, not `~/...`) and run `docker compose up -d`. |
| Map is empty, `/tiles/...` answers 404 | Same as above, for the `web` container. |
| Module upload: `signature` check fails | The module was signed with a different key than the one the server was built with. Sign it with your `owner.secret` (step 6). |
