# rfmap/tiles-builder

This Docker image builds all the map data FPV Radio Map serves from its own server. At runtime nothing comes from a third‑party map service. The weather forecast is the only exception, and this tool does not touch it.

```
docker build -t rfmap/tiles-builder tools/tiles-builder
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/data:/data" rfmap/tiles-builder all \
    --bbox 34.8,48.3,35.4,48.7 --region europe/ukraine
```

Admin step-by-step guide: [`docs/admin/tiles-builder.md`](../../docs/admin/tiles-builder.md).

## Commands

| command | output (in `/data/out`) | source |
|---|---|---|
| `basemap` | `basemap.pmtiles`: vector, OpenMapTiles schema, z0–16 | OSM `.osm.pbf` (`--region` Geofabrik id/path, `--osm-url`, or a local `--osm-file`) |
| `dem` | `dem.cog.tif` (**calculation DEM**), `hillshade.pmtiles`, `contours.pmtiles` | Copernicus DEM GLO-30 (AWS `copernicus-dem-30m`, no auth); `--dem-source srtm` / local `--dem-file` |
| `landcover` | `landcover.cog.tif` (clutter classes) | ESA WorldCover 10 m 2021 v200 (AWS, no auth) |
| `satellite` | `satellite.pmtiles`: raster | `--sat-provider` (a built-in XYZ mosaic), `--sat-file` (a local raster of a listed mosaic) or `--sat-source` (any XYZ template or raster); you choose it, see licences |
| `all` | everything above | |
| `style` | `style-dark.json` + `fonts/` | `styles/style-dark.template.json` |
| `manifest` | `manifest.json` | the per-file records in `out/.meta/` |
| `verify` | — | GDAL COG validator + `pmtiles verify` |

`style-dark.json` and `manifest.json` are rewritten after every command. Run `rfmap-tiles <command> --help` to see all options.

Directory layout: the data directory (`--data`, default `$RFMAP_DATA` or the current directory, which is `/data` in the image) holds `cache/` (downloads, reused between runs), `work/` (temporary files) and `out/` (the results; `--out` moves them). `inputs/` is a convenient place for files you supply: a bare file name that is not found in the current directory is looked up there.

**Command-line values are validated before use** (the tool may be run by scripts or AI agents, so its arguments are treated as untrusted):
- Paths (`--data`, `--out` and every `--…-file`) are resolved with `realpath` and must lie inside the directory the build is started from (`/data` in the image). They may use only letters, digits and `_ . , / : = + -`.
- Text and numbers that reach a tool's command line must match that same character set and may not start with `-`, so they cannot add options to the command. `--java-heap` takes a size such as `4g`; `--region` takes lower-case letters, digits, `_`, `-` and `/`.
- URLs (`--osm-url`, `--sat-source`) must be `https://`, contain no spaces or quotes, and resolve (here, not only through a proxy) to public addresses. The host is resolved and checked once, and every connection then goes to that checked address, also through an http proxy, which is asked to CONNECT to the address rather than the name (`https_proxy` must then be a full proxy URL with the http scheme). Files are downloaded this way with `curl --connect-to`; XYZ tiles go over reused connections from one lookup per job. Redirects are not followed: a URL that redirects fails, so pass the final URL. A raster given by URL is downloaded into `cache/` first. Downloads from the fixed sources follow https redirects.

**Idempotent:** every output is written under a temporary name and renamed into place only when it is finished. A step whose output already exists, was built with the same parameters and has the same sha256 is skipped. `--force` rebuilds it.

## Design decisions

**Planetiler (not tilemaker) for the basemap.** It is a single pinned JAR with the OpenMapTiles profile built in, so there is no style schema to maintain. It writes `.pmtiles` directly and is the fastest of the open tools: a small country takes minutes, and it scales to the whole planet. tilemaker would need a C++ build plus our own Lua/JSON profile. Planetiler 0.10.2 allows `--maxzoom` up to 16, so the default is z0–16 as specified. The OpenMapTiles schema carries no extra detail beyond z14, so `--basemap-maxzoom 14` gives a much smaller file that MapLibre over-zooms with the same look.

**Calculation DEM = source pixels, bit for bit.**
- Copernicus tiles are mosaicked with a VRT and cut with an integer pixel window (`-srcwin`). There is no `gdalwarp` and no `-projwin` rounding.
- After writing, every pixel of the COG is compared with the source (`verified: all N×M pixels … identical`). The build fails if any pixel differs.
- Output format:
  - Float32, NoData −32768, used only where a tile does not exist.
  - Internal tiles 512×512, DEFLATE + floating-point predictor (lossless).
  - Overviews are averaged; they are for previews only, and calculations read the full-resolution level.
- Copernicus widens its longitude spacing above 50° latitude. A bbox that spans two spacing zones is **refused** unless `--allow-dem-resample` is given. With that flag only the coarser tiles are warped once, nearest-neighbour, onto the finest grid with one `gdalwarp` per coarse tile (the only warping in the DEM step), so their pixels are duplicated, never interpolated. Tiles already on that grid stay bit for bit, the COG is still verified pixel by pixel against the mosaic, and the manifest records the duplication.

**Hillshade and contours are display only.** Both are derived from `dem.cog.tif`.
- Hillshade: the DEM is reprojected to Web Mercator with bilinear resampling. It uses `gdaldem -multidirectional`, and the scale is corrected by cos(latitude) at the bbox centre. The default output is a transparent PNG with black shadows and faint white highlights, so it overlays the dark basemap without greying it out. `--hillshade-style gray` gives classic grey.
- Contours: `gdal_contour` runs on the unmodified COG. The output has two source layers, `contours` (all contours, from z12) and `contours_index` (every 5th, from z10). Both carry the attribute `ele` (m).
- **The server's calculations read `dem.cog.tif` directly**, never these tiles.

**Landcover** works the same way as the DEM: the source window is copied, class values are unchanged, and the result is verified pixel by pixel. From each 3°×3° WorldCover COG only the window inside the bbox is read over HTTP and kept in `data/cache`, one file per tile. A failed request is retried, and a rerun after a failure downloads only the tiles that are still missing. Overviews use MODE. Classes: 10 tree, 20 shrub, 30 grass, 40 crop, 50 built-up, 60 bare, 70 snow/ice, 80 water, 90 wetland, 95 mangrove, 100 moss/lichen, 0 no data.

**Satellite source is your choice, and there is no default.**
- `--sat-provider` picks an XYZ mosaic from the built-in list: `eox-s2cloudless-2016` (EOX notes CC BY 4.0) and `eox-s2cloudless-2018` … `-2023` (EOX notes CC BY-NC-SA 4.0, non-commercial). Tiles are copied byte for byte, rate-limited (`--sat-rate`) and capped (`--sat-max-tiles`). An interrupted download resumes.
- `--sat-file` takes a local GeoTIFF/COG/VRT, reprojects it and writes JPEG tiles. `--sat-mosaic` says which listed mosaic the file holds: one of the EOX entries or `copernicus-sentinel-2` (any Sentinel-2 imagery; also pass `--sat-data-year`).
- `--sat-source` takes any other mosaic: an https XYZ template with `{z}` `{x}` `{y}` (or `{-y}` for TMS rows), copied byte for byte like a provider, or a GDAL raster as a local path or an https URL. Give its texts with `--sat-name`, `--sat-licence` and `--sat-attribution`; these three also override the table's texts for a listed mosaic.
- **Licences differ between Sentinel-2 cloudless mosaics, and many tile services forbid bulk downloading.** The operator must check the chosen mosaic's licence and terms (the site is non-commercial) and confirm that with `--sat-licence-checked` (or by passing both `--sat-licence` and `--sat-attribution`). Without it the manifest marks the licence `UNVERIFIED`.

**PMTiles packaging.** GDAL (raster MBTiles, vector MVT) → `pmtiles convert`. The header zoom range is taken from the tiles that actually exist, so the archives pass `pmtiles verify`.

## Pinned versions (Dockerfile)

| | version |
|---|---|
| base | `ubuntu:24.04@sha256:ac3a3d13…1594` |
| Ubuntu packages | snapshot `20261015T000000Z`: gdal-bin / python3-gdal `3.8.4+dfsg-3ubuntu3`, openjdk-21-jre-headless `21.0.12.1+1-1~24.04.4`, curl, git, python3, sqlite3 (exact versions) |
| Planetiler | 0.10.2 (sha256 checked) |
| go-pmtiles | 1.31.0 (sha256 checked, amd64/arm64) |
| glyphs | protomaps/basemaps-assets @ `028c18f7…` (Noto Sans, SIL OFL 1.1) |

**Upgrading pinned tools:** change the `ARG`s and the sha256 values, then rebuild and run `tests/smoke.sh`. The Ubuntu snapshot date controls which package versions exist. Pick a newer date and copy the versions from `apt-cache policy` inside `ubuntu:24.04` after `apt-get update --snapshot <date>`.

Behind a TLS-intercepting proxy, build with `--secret id=extra_ca,src=/path/ca.crt`. The certificate is used only during the build and is not left in the image.

## Test

`tests/smoke.sh [DATA_DIR]` runs `all` for a 0.1°×0.06° box near 48.53 N 35.20 E with:
- Planetiler's own Monaco fixtures for the basemap;
- one Sentinel-2 L2A true-colour scene from the public `sentinel-cogs` bucket, downloaded into `inputs/` and used as `--sat-file`. This scene is a test stand-in, **not** a cloudless mosaic.

It then re-runs `all` (every step must report "up to date") and runs `verify`.

`tests/raster_test.py` checks the DEM mosaic across 50° N with synthetic Copernicus-like tiles, which the smoke box does not reach. It needs the GDAL Python bindings, so run it in the image from the repository root:

```bash
docker run --rm -v "$PWD/tools/tiles-builder/tests:/tests:ro" --entrypoint python3 rfmap/tiles-builder -m unittest discover -s /tests -p '*_test.py'
```

## Source attribution (show on the map)

- `© OpenMapTiles © OpenStreetMap contributors` (ODbL; OpenMapTiles schema CC BY 4.0)
- `Copernicus DEM GLO-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved`
- `© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium` (CC BY 4.0)
- satellite: the attribution of the chosen mosaic from `SATELLITE_MOSAICS`
- glyph fonts: Noto Sans, SIL Open Font License 1.1 (`out/fonts/OFL.txt`)

`manifest.json → attribution_display_text` has these strings joined together. Each source's attribution is also set in `style-dark.json`, so MapLibre's attribution control shows it.
