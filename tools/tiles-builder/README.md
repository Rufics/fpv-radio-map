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
| `basemap` | `basemap.pmtiles`: vector, OpenMapTiles schema, z0–16 | OSM `.osm.pbf` (`--region` Geofabrik id/path, `--osm-url`, `--osm-file`) |
| `dem` | `dem.cog.tif` (**calculation DEM**), `hillshade.pmtiles`, `contours.pmtiles` | Copernicus DEM GLO-30 (AWS `copernicus-dem-30m`, no auth); `--dem-source srtm` / `--dem-file` |
| `landcover` | `landcover.cog.tif` (clutter classes) | ESA WorldCover 10 m 2021 v200 (AWS, no auth) |
| `satellite` | `satellite.pmtiles`: raster | `--sat-source`: an XYZ template or a GDAL raster URL/path (you choose it, see licences) |
| `all` | everything above | |
| `style` | `style-dark.json` + `fonts/` | `styles/style-dark.template.json` |
| `manifest` | `manifest.json` | the per-file records in `out/.meta/` |
| `verify` | — | GDAL COG validator + `pmtiles verify` |

`style-dark.json` and `manifest.json` are rewritten after every command. Run `rfmap-tiles <command> --help` to see all options.

Directory layout: `/data/cache` holds downloads and is reused between runs. `/data/work` holds temporary files. `/data/out` holds the results.

**Checked inputs:** URLs must be `https://` (redirects too), and a satellite source must resolve to public addresses only. Local files must exist and are passed on as absolute paths. `--java-heap` takes a size such as `4g`.

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
- Copernicus widens its longitude spacing above 50° latitude. A bbox that spans two spacing zones is **refused** unless `--allow-dem-resample` is given. That flag duplicates pixels nearest-neighbour and records it in the manifest.

**Hillshade and contours are display only.** Both are derived from `dem.cog.tif`.
- Hillshade: the DEM is reprojected to Web Mercator with bilinear resampling. It uses `gdaldem -multidirectional`, and the scale is corrected by cos(latitude) at the bbox centre. The default output is a transparent PNG with black shadows and faint white highlights, so it overlays the dark basemap without greying it out. `--hillshade-style gray` gives classic grey.
- Contours: `gdal_contour` runs on the unmodified COG. The output has two source layers, `contours` (all contours, from z12) and `contours_index` (every 5th, from z10). Both carry the attribute `ele` (m).
- **The server's calculations read `dem.cog.tif` directly**, never these tiles.

**Landcover** works the same way as the DEM: the source window is copied, class values are unchanged, and the result is verified pixel by pixel for areas up to 50 Mpx. It reads only the needed blocks of the 3°×3° WorldCover COGs over HTTP. Overviews use MODE. Classes: 10 tree, 20 shrub, 30 grass, 40 crop, 50 built-up, 60 bare, 70 snow/ice, 80 water, 90 wetland, 95 mangrove, 100 moss/lichen, 0 no data.

**Satellite source is configurable, and there is no default.**
- XYZ mode (`{z}/{x}/{y}`, `{-y}` for TMS rows) copies tiles byte for byte, rate-limited (`--sat-rate`) and capped (`--sat-max-tiles`). An interrupted download resumes.
- Raster mode (a GeoTIFF/COG/VRT path or URL) reprojects the image and writes JPEG tiles.
- **Licences differ between Sentinel-2 cloudless mosaics, and many tile services forbid bulk downloading.** The operator must check the chosen mosaic's licence and terms (the site is non-commercial) and pass `--sat-licence` and `--sat-attribution`. Without them the manifest marks the licence `UNVERIFIED`. This tool does not claim any licence for any mosaic.

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
- one Sentinel-2 L2A true-colour scene from the public `sentinel-cogs` bucket as the satellite source. This scene is a test stand-in, **not** a cloudless mosaic.

It then re-runs `all` (every step must report "up to date") and runs `verify`.

## Source attribution (show on the map)

- `© OpenMapTiles © OpenStreetMap contributors` (ODbL; OpenMapTiles schema CC BY 4.0)
- `Copernicus DEM GLO-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved`
- `© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium` (CC BY 4.0)
- satellite: the text you verified and passed with `--sat-attribution`
- glyph fonts: Noto Sans, SIL Open Font License 1.1 (`out/fonts/OFL.txt`)

`manifest.json → attribution_display_text` has these strings joined together. Each source's attribution is also set in `style-dark.json`, so MapLibre's attribution control shows it.
