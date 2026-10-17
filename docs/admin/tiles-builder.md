# Building map data (rfmap/tiles-builder)

This guide is for the person who runs the FPV Radio Map server. It produces the map files the site serves from our own server: basemap, hillshade, contours, satellite, the calculation DEM and the landcover (clutter) raster. Tool details are in [`tools/tiles-builder/README.md`](../../tools/tiles-builder/README.md).

> Estimates in this guide are marked **(estimate)**. They come from small test runs and the tools' general behaviour, not from measuring a full country.

## 1. Install Docker

- Linux (Debian/Ubuntu): follow <https://docs.docker.com/engine/install/>, then `sudo usermod -aG docker $USER` and log in again.
- Windows/macOS: install Docker Desktop. In Settings → Resources, give it at least 8 GB RAM for a country-sized build.
- Check that it works: `docker info` must print a `Server:` section without errors.

## 2. Build the image (once, and after each update of the repo)

```bash
cd fpv-radio-map
docker build -t rfmap/tiles-builder tools/tiles-builder
```

The first build takes a few minutes and downloads about 350 MB **(estimate)**. All tool versions are pinned. Behind a company proxy that intercepts TLS, add `--secret id=extra_ca,src=/path/to/proxy-ca.crt`.

## 3. Choose the area

You need two things:

1. **A bbox** for the elevation, landcover and satellite data: `minLon,minLat,maxLon,maxLat` in degrees. Example: `34.8,48.3,35.4,48.7`. To find coordinates, draw a rectangle at <https://boundingbox.klokantech.com> and choose "CSV".
2. **An OSM region** for the basemap: a Geofabrik id such as `monaco` or `ukraine`, or a path such as `europe/ukraine` (browse <https://download.geofabrik.de>). When a bbox is also given, the basemap is clipped to it. Add `--basemap-full-extract` to keep the whole region.

Rules:
- Areas that cross the 180° meridian are not supported.
- Copernicus DEM uses a wider pixel spacing north of 50° latitude. If your bbox crosses 50° N (or 60°, 70°, 80°), the tool stops instead of silently resampling. Split the area at that latitude, or accept duplicated pixels with `--allow-dem-resample`.

## 4. Run

```bash
mkdir -p data
docker run --rm -v "$PWD/data:/data" rfmap/tiles-builder all \
    --bbox 34.8,48.3,35.4,48.7 \
    --region europe/ukraine
```

You can also run the steps one at a time with the same options: `basemap`, `dem`, `landcover`, `satellite`. Useful options:

| option | meaning |
|---|---|
| `--contour-interval 10` | contour spacing in m (default 20) |
| `--basemap-maxzoom 14` | much smaller basemap that looks the same (the map over-zooms) |
| `--dem-source srtm` | fallback DEM (AWS Terrain Tiles, SRTM-based) if Copernicus is not reachable |
| `--dem-file my.tif` | use your own DEM files (repeatable) |
| `--java-heap 6g` | memory for the basemap step |
| `--public-base-url https://maps.example.org/tiles` | URL written into the style (default `/tiles`) |
| `--force` | rebuild even if nothing changed |

**Satellite** is not built unless you choose a source:

```bash
docker run --rm -v "$PWD/data:/data" rfmap/tiles-builder satellite \
    --bbox 34.8,48.3,35.4,48.7 \
    --sat-source 'https://<provider>/<layer>/{z}/{y}/{x}.jpg' \
    --sat-name 'Sentinel-2 cloudless <year> (<provider>)' \
    --sat-licence '<licence you checked>' \
    --sat-attribution '<exact attribution text the licence requires>'
```

- **Check the licence yourself before publishing.** Sentinel-2 cloudless mosaics are published under different licences per provider and per year. Some allow only non-commercial use; some tile services forbid bulk downloading altogether. Read the provider's licence and terms of service. If they offer the mosaic as a downloadable GeoTIFF, prefer that and pass the file path or URL as `--sat-source`.
- We could not verify any provider's terms while writing this tool, so it names no recommended mosaic. If you leave out `--sat-licence` or `--sat-attribution`, the manifest records the licence as `UNVERIFIED`.
- The download is rate-limited (`--sat-rate`, default 4 requests/s) and refuses jobs above `--sat-max-tiles`.

Running the same command again is safe. Finished outputs with unchanged inputs are skipped. Interrupted downloads resume from `data/cache`.

## 5. Check the result

```bash
docker run --rm -v "$PWD/data:/data" rfmap/tiles-builder verify
```

Expect `OK` for every file. In `data/out` you get:

```
basemap.pmtiles  hillshade.pmtiles  contours.pmtiles  satellite.pmtiles
dem.cog.tif      landcover.cog.tif  style-dark.json   manifest.json   fonts/
```

`manifest.json` lists each file with its source, licence, attribution, bbox, resolution, build date, sha256 and the tool versions.

## 6. Copy to the server

Copy the **whole `out/` directory**, including `fonts/`, into the server's tiles data directory. The web app expects it under the URL `/tiles/`.

```bash
rsync -av --delete data/out/ server:/srv/rfmap/data/tiles/
```

`/srv/rfmap/data/tiles` is an example path. Use the data directory configured for your server.

- `dem.cog.tif` and `landcover.cog.tif` are what the radio calculations read. They read them directly from disk. The hillshade and contour tiles are only pictures.
- The web server must serve `*.pmtiles` with HTTP **Range** requests. nginx and Caddy static file serving do this by default.
- If the tiles are on a different host than the web app, enable CORS for `GET` and `HEAD` with the `Range` header.
- Do not put a compressing proxy in front of `.pmtiles`. Disable gzip for them, because PMTiles are already compressed internally.
- After copying, open the map and check that the attribution text (step 8) is visible.

## 7. Disk, RAM and time — all **(estimate)**

| item | per unit | notes |
|---|---|---|
| DEM COG | ~25–45 MB per 1°×1° | a 0.5°×0.3° test area was 6.4 MB; mountains compress less |
| Copernicus download cache | ~20–40 MB per 1°×1° tile | kept in `data/cache` |
| hillshade z6–12 | ~10–20 MB per 1°×1° | test area: 2.1 MB for 0.5°×0.3° |
| contours (20 m) | ~1–20 MB per 1°×1° | flat land is small, mountains are large |
| landcover COG | ~10–40 MB per 1°×1° | only the needed blocks are downloaded |
| satellite z6–14 | ~100–200 MB per 1°×1° | depends on the provider's JPEG quality |
| basemap z0–16 | roughly 1–2× the `.osm.pbf` size | about half of that with `--basemap-maxzoom 14` |
| basemap build | RAM ≥ 2–3× the `.pbf` size (min 2 GB); temporary disk ~5–10× the `.pbf` | a country of 1–2 GB `.pbf`: 8–16 GB RAM, 10–30 min on 4–8 cores |
| DEM + derived for 1°×1° | a few minutes on 4 cores | the satellite download time is set by `--sat-rate` |

The smoke test is a 0.1°×0.06° box plus Monaco. It ran in about 30 s on 4 cores, with all downloads included.

## 8. Attribution to show on the map

These are required by the data licences. The style already sets each one on its source, so MapLibre's attribution control shows them. The combined text is also in `manifest.json → attribution_display_text`.

- **© OpenMapTiles © OpenStreetMap contributors**: OSM data under ODbL 1.0, linked to <https://www.openstreetmap.org/copyright>; OpenMapTiles schema under CC BY 4.0.
- **Copernicus DEM GLO-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved**
- **© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium** (CC BY 4.0). The clutter layer comes from this data.
- **Satellite:** exactly the text you passed with `--sat-attribution`.
- SRTM fallback, if used: *Elevation: SRTM (NASA/USGS) and other sources via Mapzen Terrain Tiles*. See <https://github.com/tilezen/joerd/blob/master/docs/attribution.md>.
- Fonts: Noto Sans, SIL Open Font License 1.1. No on-map credit is needed; keep `fonts/OFL.txt` with the files.

Before going public, check the current licence pages of Copernicus DEM, ESA WorldCover and your satellite mosaic. These texts were correct to our knowledge when we wrote this guide, but providers can change them.

## 9. Updating

- **New OSM data** (weekly or monthly is plenty): run `basemap` again with `--refresh-osm`. Without it, the cached extract is reused.
- **DEM and landcover**: these sources rarely change. Rebuild only when you change the area: run `dem` and `landcover` with the new `--bbox`. Downloaded tiles are reused from the cache.
- **New tool versions**: `git pull`, `docker build …` again, then run `all --force`.
- After each update, run `verify` and copy `out/` to the server again (step 6). Clients pick up new files on their next load.
- To free disk space, delete `data/cache` and `data/work`. Never delete them while a build is running.

## 10. Known limitations and honest notes

- **Geofabrik** (OSM extracts), **osmdata.openstreetmap.de** (water polygons) and **naciscdn.org** (Natural Earth) need direct internet access. They require no credentials. If your network blocks them, download the files elsewhere and pass `--osm-file`, `--water-polygons-file` and `--natural-earth-file`.
- **Copernicus DEM** comes from the public AWS bucket, with no account needed. Some tiles are missing from the public GLO-30 release: open ocean, and a few countries the provider withholds. Missing tiles become NoData, never invented heights, and are listed under `missing_tiles` in the manifest.
- **Copernicus is a surface model.** It includes buildings and tree canopy. Heights are relative to the EGM2008 geoid.
- **The SRTM fallback** uses AWS Terrain Tiles, a blend based mostly on SRTM. The original NASA SRTMGL1 tiles need a free NASA Earthdata login. Download those yourself and use `--dem-file`.
- **Hillshade and satellite at low zoom:** for very small areas they start at a higher zoom, because the lowest levels would be smaller than one tile.
