"""satellite: a Sentinel-2 cloudless mosaic -> raster PMTiles.

The source is configurable because licences differ between mosaics:
  * an XYZ/WMTS tile URL template containing {z} {x} {y} (or {-y} for TMS rows):
    tiles are copied byte-for-byte into the archive (no re-encoding);
  * any GDAL-readable raster (local path, or http(s) URL of a GeoTIFF/COG/VRT):
    reprojected to Web Mercator and cut into JPEG tiles.
No source is enabled by default: the operator must pick one, check its licence
and terms of use (bulk tile downloading is forbidden by many tile services),
and pass --sat-licence / --sat-attribution.
"""
from __future__ import annotations

import hashlib
import math
import shutil
import sqlite3
import time
import urllib.error
import urllib.request
from pathlib import Path

from . import sources
from .common import (USER_AGENT, BBox, BuildError, Dirs, fresh_dir, install, is_up_to_date, log,
                     mbtiles_set_metadata, mbtiles_to_pmtiles, overview_factors, pmtiles_header, run,
                     tile_aligned_extent, warn, write_meta, zoom_resolution)

OUT_NAME = "satellite.pmtiles"


def add_arguments(p) -> None:
    g = p.add_argument_group("satellite")
    g.add_argument("--sat-source",
                   help="XYZ template, e.g. 'https://host/path/{z}/{y}/{x}.jpg', or a GDAL raster "
                        "path/URL (GeoTIFF/COG/VRT). Required for the satellite step.")
    g.add_argument("--sat-name", default="Sentinel-2 cloudless mosaic", help="human-readable source name")
    g.add_argument("--sat-licence", default=None, help="licence of the chosen mosaic, as verified by you")
    g.add_argument("--sat-attribution", default=None, help="attribution text required by the mosaic licence")
    g.add_argument("--sat-minzoom", type=int, default=6)
    g.add_argument("--sat-maxzoom", type=int, default=14, help="default 14 (~6-10 m/px; Sentinel-2 is 10 m)")
    g.add_argument("--sat-rate", type=float, default=4.0, help="XYZ mode: max requests per second (default 4)")
    g.add_argument("--sat-max-tiles", type=int, default=50000, help="XYZ mode: refuse jobs above this tile count")
    g.add_argument("--sat-jpeg-quality", type=int, default=85, help="raster mode: JPEG quality")


def _tile_range(bbox: BBox, z: int) -> tuple[int, int, int, int]:
    n = 2 ** z
    def tx(lon): return min(n - 1, max(0, int((lon + 180) / 360 * n)))
    def ty(lat):
        r = math.radians(lat)
        return min(n - 1, max(0, int((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n)))
    return tx(bbox.min_lon), ty(bbox.max_lat), tx(bbox.max_lon), ty(bbox.min_lat)


def _format_of(data: bytes) -> str | None:
    if data[:3] == b"\xff\xd8\xff":
        return "jpg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    return None


def _open_mbtiles(path: Path) -> sqlite3.Connection:
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE IF NOT EXISTS metadata (name TEXT, value TEXT)")
    con.execute("CREATE TABLE IF NOT EXISTS tiles (zoom_level INTEGER, tile_column INTEGER, "
                "tile_row INTEGER, tile_data BLOB)")
    con.execute("CREATE UNIQUE INDEX IF NOT EXISTS tile_index ON tiles (zoom_level, tile_column, tile_row)")
    return con


def _xyz(args, dirs: Dirs, bbox: BBox, mb: Path) -> dict:
    tpl = args.sat_source
    zmin, zmax = args.sat_minzoom, args.sat_maxzoom
    jobs = []
    for z in range(zmin, zmax + 1):
        x0, y0, x1, y1 = _tile_range(bbox, z)
        jobs += [(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
    if len(jobs) > args.sat_max_tiles:
        raise BuildError(f"{len(jobs)} tiles needed (> --sat-max-tiles {args.sat_max_tiles}); "
                         "lower --sat-maxzoom or raise the limit after checking the provider's terms")
    log(f"XYZ: {len(jobs)} tiles z{zmin}-{zmax}, max {args.sat_rate:g} req/s")
    con = _open_mbtiles(mb)
    have = {(z, x, (2 ** z - 1) - r) for z, x, r in con.execute("SELECT zoom_level, tile_column, tile_row FROM tiles")}
    fmt, fetched, missing = None, 0, 0
    delay = 1.0 / args.sat_rate if args.sat_rate > 0 else 0
    for i, (z, x, y) in enumerate(jobs):
        if (z, x, y) in have:
            continue
        url = (tpl.replace("{z}", str(z)).replace("{x}", str(x))
               .replace("{-y}", str(2 ** z - 1 - y)).replace("{y}", str(y)))
        t0 = time.monotonic()
        data = None
        for attempt in range(5):
            try:
                req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=60) as r:
                    data = r.read()
                break
            except urllib.error.HTTPError as e:
                if e.code in (204, 404):
                    break
                if e.code in (401, 403):
                    raise BuildError(f"{url}: HTTP {e.code} — the service refuses access "
                                     "(credentials or terms of use?)")
                if attempt == 4:
                    raise BuildError(f"{url}: HTTP {e.code}")
            except (urllib.error.URLError, TimeoutError) as e:
                if attempt == 4:
                    raise BuildError(f"{url}: {e}")
            time.sleep(2 ** attempt)
        if data is None:
            missing += 1
        else:
            f = _format_of(data)
            if f is None:
                raise BuildError(f"{url}: response is not a JPEG/PNG/WebP image")
            if fmt and f != fmt:
                warn(f"mixed tile formats ({fmt}, {f}); MapLibre handles both, metadata says {fmt}")
            fmt = fmt or f
            con.execute("INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)", (z, x, 2 ** z - 1 - y, data))
            fetched += 1
            if fetched % 200 == 0:
                con.commit()
                log(f"  {i + 1}/{len(jobs)} tiles")
        wait = delay - (time.monotonic() - t0)
        if wait > 0:
            time.sleep(wait)
    con.commit()
    if fmt is None:
        row = con.execute("SELECT tile_data FROM tiles LIMIT 1").fetchone()
        fmt = _format_of(row[0]) if row else None
    if fmt is None:
        raise BuildError("no tiles were downloaded")
    con.close()
    mbtiles_set_metadata(mb, {"name": "satellite", "format": fmt, "minzoom": zmin, "maxzoom": zmax,
                              "bounds": str(bbox), "type": "baselayer"})
    log(f"XYZ: fetched {fetched}, already cached {len(have)}, not available {missing}")
    return {"mode": "xyz", "tiles_fetched": fetched, "tiles_missing": missing, "format": fmt}


def _raster(args, dirs: Dirs, bbox: BBox, mb: Path) -> dict:
    src = args.sat_source
    if src.startswith(("http://", "https://")):
        src = "/vsicurl/" + src
    from . import raster
    i = raster.info(src)
    bands = i["bands"]
    if len(bands) < 3:
        raise BuildError("raster satellite source needs at least 3 bands (RGB)")
    zmin, zmax = args.sat_minzoom, args.sat_maxzoom
    work = mb.parent
    res = zoom_resolution(zmax)
    te = tile_aligned_extent(bbox, zmax)
    merc = work / "sat3857.tif"
    run(["gdalwarp", "-t_srs", "EPSG:3857", "-r", "cubic", "-tr", repr(res), repr(res), "-te", *map(repr, te),
         "-multi", "-wo", "NUM_THREADS=ALL_CPUS", "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE",
         "-co", "BIGTIFF=IF_SAFER", src, str(merc)],
        env={"GDAL_DISABLE_READDIR_ON_OPEN": "EMPTY_DIR", "GDAL_HTTP_MAX_RETRY": "5"})
    rgb = work / "sat-rgb.vrt"
    tr = ["gdal_translate", "-of", "VRT", "-b", "1", "-b", "2", "-b", "3"]
    if bands[0]["type"] != "Byte":
        warn(f"source is {bands[0]['type']}; stretching to 8 bit with -scale (check the colours)")
        tr += ["-ot", "Byte", "-scale"]
    run(tr + [str(merc), str(rgb)])
    run(["gdal_translate", "-of", "MBTiles", "-co", "TILE_FORMAT=JPEG", "-co", f"QUALITY={args.sat_jpeg_quality}",
         "-co", "NAME=satellite", str(rgb), str(mb)])
    if zmax > zmin:
        run(["gdaladdo", "-r", "average", str(mb), *overview_factors(zmin, zmax)])
    return {"mode": "raster", "format": "jpg", "resampling": "cubic (display only)"}


def build(args, dirs: Dirs) -> None:
    if not args.bbox:
        raise BuildError("satellite needs --bbox")
    if not args.sat_source:
        raise BuildError("satellite needs --sat-source (see README: pick a mosaic and check its licence)")
    bbox = BBox.parse(args.bbox)
    if not (0 <= args.sat_minzoom <= args.sat_maxzoom <= 18):
        raise BuildError("need 0 <= --sat-minzoom <= --sat-maxzoom <= 18")
    xyz = "{z}" in args.sat_source
    params = {"bbox": str(bbox), "source": args.sat_source, "zoom": [args.sat_minzoom, args.sat_maxzoom],
              "licence": args.sat_licence, "attribution": args.sat_attribution}
    if is_up_to_date(dirs, OUT_NAME, params, args.force):
        return
    if not args.sat_licence or not args.sat_attribution:
        warn(sources.SATELLITE_UNVERIFIED)

    key = hashlib.sha256(f"{args.sat_source}|{bbox}|{args.sat_minzoom}|{args.sat_maxzoom}".encode()).hexdigest()[:12]
    if xyz:
        # Kept between runs so an interrupted download resumes.
        work = dirs.cache / "satellite-xyz" / key
        work.mkdir(parents=True, exist_ok=True)
        mb = work / "tiles.mbtiles"
        detail = _xyz(args, dirs, bbox, mb)
    else:
        work = fresh_dir(dirs.work / "satellite")
        mb = work / "satellite.mbtiles"
        detail = _raster(args, dirs, bbox, mb)

    attribution = args.sat_attribution or f"{args.sat_name} (attribution not set)"
    mbtiles_set_metadata(mb, {"attribution": attribution, "description": args.sat_name})
    tmp = dirs.out / f".tmp-{OUT_NAME}"
    mbtiles_to_pmtiles(mb, tmp)
    install(tmp, dirs, OUT_NAME)
    h = pmtiles_header(dirs.out / OUT_NAME)
    write_meta(dirs, OUT_NAME, params, {
        "kind": "raster-tiles",
        "source": f"{args.sat_name}: {args.sat_source}",
        "licence": args.sat_licence or sources.SATELLITE_UNVERIFIED,
        "attribution": attribution,
        "licence_verified_by_operator": bool(args.sat_licence and args.sat_attribution),
        "bbox": bbox.as_list(),
        "zoom": [h.get("minzoom", args.sat_minzoom), h.get("maxzoom", args.sat_maxzoom)],
        "resolution": f"z{args.sat_maxzoom} = {zoom_resolution(args.sat_maxzoom):.1f} m/px (EPSG:3857 units)",
        **detail,
    })
    if not xyz:
        shutil.rmtree(work, ignore_errors=True)
