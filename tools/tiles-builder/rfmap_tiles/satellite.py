"""satellite: a Sentinel-2 cloudless mosaic -> raster PMTiles.

The source is chosen by the operator because licences differ between mosaics:
  * --sat-provider: one of the XYZ mosaics in sources.SATELLITE_PROVIDERS; tiles are copied
    byte-for-byte into the archive (no re-encoding);
  * --sat-file: a GDAL-readable raster (GeoTIFF/COG/VRT) in /data/inputs, reprojected to Web
    Mercator and cut into JPEG tiles. Any other mosaic is downloaded into /data/inputs first.
No source is enabled by default: the operator must pick one, check its licence
and terms of use (bulk tile downloading is forbidden by many tile services),
and confirm that with --sat-licence-checked. Name, licence and attribution come from
sources.SATELLITE_MOSAICS; a mosaic that is not listed there is added there first.
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
from .common import (USER_AGENT, BBox, BuildError, Dirs, fresh_dir, input_file, inputs_entry, install, is_up_to_date, log,
                     mbtiles_set_metadata, mbtiles_to_pmtiles, overview_factors, pmtiles_header, run,
                     tile_aligned_extent, warn, write_meta, zoom_resolution)

OUT_NAME = "satellite.pmtiles"


def add_arguments(p) -> None:
    g = p.add_argument_group("satellite")
    g.add_argument("--sat-provider", choices=sorted(sources.SATELLITE_PROVIDERS),
                   help="XYZ mosaic to download tiles from (see README for each one's licence)")
    g.add_argument("--sat-file", type=input_file,
                   help="a GDAL raster (GeoTIFF/COG/VRT) in /data/inputs; needs --sat-mosaic")
    g.add_argument("--sat-mosaic", choices=sorted(sources.SATELLITE_MOSAICS),
                   help="which mosaic --sat-file holds; its name, licence and attribution come from the table")
    g.add_argument("--sat-data-year", type=int, default=None,
                   help="acquisition year of the imagery, for mosaics whose attribution names it")
    g.add_argument("--sat-licence-checked", action="store_true",
                   help="you checked the mosaic's current licence and terms of use")
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


def _xyz_jobs(bbox: BBox, zmin: int, zmax: int, max_tiles: int) -> list[tuple[int, int, int]]:
    jobs = []
    for z in range(zmin, zmax + 1):
        x0, y0, x1, y1 = _tile_range(bbox, z)
        jobs += [(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
    if len(jobs) > max_tiles:
        raise BuildError(f"{len(jobs)} tiles needed (> --sat-max-tiles {max_tiles}); "
                         "lower --sat-maxzoom or raise the limit after checking the provider's terms")
    return jobs


def _tile_url(tpl: str, z: int, x: int, y: int) -> str:
    return (tpl.replace("{z}", str(z)).replace("{x}", str(x))
            .replace("{-y}", str(2 ** z - 1 - y)).replace("{y}", str(y)))


def _get(url: str) -> bytes | None:
    """One request: the tile's bytes, or None when the service has no tile there."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (204, 404):
            return None
        if e.code in (401, 403):
            raise BuildError(f"{url}: HTTP {e.code} — the service refuses access "
                             "(credentials or terms of use?)")
        raise


def _fetch_tile(url: str) -> bytes | None:
    """The tile's bytes, or None when the service has no tile there; retries transient errors."""
    for attempt in range(4):
        try:
            return _get(url)
        except (urllib.error.URLError, TimeoutError):
            time.sleep(2 ** attempt)
    try:
        return _get(url)
    except urllib.error.HTTPError as e:
        raise BuildError(f"{url}: HTTP {e.code}")
    except (urllib.error.URLError, TimeoutError) as e:
        raise BuildError(f"{url}: {e}")


def _checked_format(url: str, data: bytes, fmt: str | None) -> str:
    """The archive's tile format after this tile: the first format seen, with a warning on a mix."""
    f = _format_of(data)
    if f is None:
        raise BuildError(f"{url}: response is not a JPEG/PNG/WebP image")
    if fmt and f != fmt:
        warn(f"mixed tile formats ({fmt}, {f}); MapLibre handles both, metadata says {fmt}")
    return fmt or f


def _xyz(tpl: str, zmin: int, zmax: int, max_tiles: int, rate: float, bbox: BBox, mb: Path) -> dict:
    jobs = _xyz_jobs(bbox, zmin, zmax, max_tiles)
    log(f"XYZ: {len(jobs)} tiles z{zmin}-{zmax}, max {rate:g} req/s")
    con = _open_mbtiles(mb)
    have = {(z, x, (2 ** z - 1) - r) for z, x, r in con.execute("SELECT zoom_level, tile_column, tile_row FROM tiles")}
    fmt, fetched, missing = None, 0, 0
    delay = 1.0 / rate if rate > 0 else 0
    for i, (z, x, y) in enumerate(j for j in jobs if j not in have):
        url = _tile_url(tpl, z, x, y)
        t0 = time.monotonic()
        data = _fetch_tile(url)
        if data is None:
            missing += 1
        else:
            fmt = _checked_format(url, data, fmt)
            con.execute("INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)", (z, x, 2 ** z - 1 - y, data))
            fetched += 1
            if fetched % 200 == 0:
                con.commit()
                log(f"  {len(have) + i + 1}/{len(jobs)} tiles")
        time.sleep(max(0.0, delay - (time.monotonic() - t0)))
    con.commit()
    if fmt is None:
        row = con.execute("SELECT tile_data FROM tiles LIMIT 1").fetchone()
        fmt = _format_of(row[0]) if row else None
    con.close()
    if fmt is None:
        raise BuildError("no tiles were downloaded")
    mbtiles_set_metadata(mb, {"name": "satellite", "format": fmt, "minzoom": zmin, "maxzoom": zmax,
                              "bounds": str(bbox), "type": "baselayer"})
    log(f"XYZ: fetched {fetched}, already cached {len(have)}, not available {missing}")
    return {"mode": "xyz", "tiles_fetched": fetched, "tiles_missing": missing, "format": fmt}


def _raster(src: Path, zmin: int, zmax: int, quality: int, bbox: BBox, mb: Path) -> dict:
    from . import raster
    i = raster.info(src)
    bands = i["bands"]
    if len(bands) < 3:
        raise BuildError("raster satellite source needs at least 3 bands (RGB)")
    work = mb.parent
    res = zoom_resolution(zmax)
    te = tile_aligned_extent(bbox, zmax)
    merc = work / "sat3857.tif"
    run(["gdalwarp", "-t_srs", "EPSG:3857", "-r", "cubic", "-tr", repr(res), repr(res), "-te", *map(repr, te),
         "-multi", "-wo", "NUM_THREADS=ALL_CPUS", "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE",
         "-co", "BIGTIFF=IF_SAFER", str(src), str(merc)])
    rgb = work / "sat-rgb.vrt"
    tr = ["gdal_translate", "-of", "VRT", "-b", "1", "-b", "2", "-b", "3"]
    if bands[0]["type"] != "Byte":
        warn(f"source is {bands[0]['type']}; stretching to 8 bit with -scale (check the colours)")
        tr += ["-ot", "Byte", "-scale"]
    run(tr + [str(merc), str(rgb)])
    run(["gdal_translate", "-of", "MBTiles", "-co", "TILE_FORMAT=JPEG", "-co", f"QUALITY={quality}",
         "-co", "NAME=satellite", str(rgb), str(mb)])
    if zmax > zmin:
        run(["gdaladdo", "-r", "average", str(mb), *overview_factors(zmin, zmax)])
    return {"mode": "raster", "format": "jpg", "resampling": "cubic (display only)"}


def _table_key(table: dict, typed: str | None) -> str:
    """The table's own key equal to the typed choice, so later code never holds the typed text."""
    return next(key for key in table if key == typed)


def _attribution(mosaic: dict, year: int | None) -> str:
    text = mosaic["attribution"]
    if "{year}" not in text:
        return text
    if year is None or not 1972 <= year <= 2100:
        raise BuildError("this mosaic's attribution names the acquisition year: pass --sat-data-year")
    return text.format(year=int(year))


def _chosen_mosaic(args) -> tuple[str, dict, str, Path | None]:
    """(table key, table entry, source description, input file) for the operator's choice.
    Every value comes from the source table or the /data/inputs listing, never the typed text."""
    if bool(args.sat_provider) == bool(args.sat_file):
        raise BuildError("satellite needs exactly one of --sat-provider or --sat-file "
                         "(see README: pick a mosaic and check its licence)")
    if args.sat_provider:
        key = _table_key(sources.SATELLITE_PROVIDERS, args.sat_provider)
        mosaic = sources.SATELLITE_PROVIDERS[key]
        return key, mosaic, mosaic["template"], None
    if not args.sat_mosaic:
        raise BuildError("--sat-file needs --sat-mosaic (which mosaic the file holds)")
    key = _table_key(sources.SATELLITE_MOSAICS, args.sat_mosaic)
    src = inputs_entry(args.sat_file.name)
    return key, sources.SATELLITE_MOSAICS[key], f"file:{src.name}", src


def _tiles(args, dirs: Dirs, key: str, source: str, src: Path | None, bbox: BBox,
           zmin: int, zmax: int) -> tuple[Path, Path, dict]:
    """Fill an MBTiles file from the chosen source; returns (mbtiles, work dir, details)."""
    if src is None:
        # Kept between runs so an interrupted download resumes; one cache per provider, area and zooms.
        area = hashlib.sha256(",".join(f"{v:.7f}" for v in bbox.as_list()).encode()).hexdigest()[:12]
        work = dirs.cache / "satellite-xyz" / f"{key}-{area}-z{zmin}-{zmax}"
        work.mkdir(parents=True, exist_ok=True)
        mb = work / "tiles.mbtiles"
        return mb, work, _xyz(source, zmin, zmax, int(args.sat_max_tiles), float(args.sat_rate), bbox, mb)
    quality = int(args.sat_jpeg_quality)
    if not 1 <= quality <= 100:
        raise BuildError("--sat-jpeg-quality must be 1-100")
    work = fresh_dir(dirs.work / "satellite")
    mb = work / "satellite.mbtiles"
    return mb, work, _raster(src, zmin, zmax, quality, bbox, mb)


def build(args, dirs: Dirs) -> None:
    if not args.bbox:
        raise BuildError("satellite needs --bbox")
    key, mosaic, source, src = _chosen_mosaic(args)
    bbox = BBox.parse(args.bbox)
    zmin, zmax = int(args.sat_minzoom), int(args.sat_maxzoom)
    if not (0 <= zmin <= zmax <= 18):
        raise BuildError("need 0 <= --sat-minzoom <= --sat-maxzoom <= 18")
    attribution = _attribution(mosaic, args.sat_data_year)
    checked = bool(args.sat_licence_checked)
    params = {"bbox": str(bbox), "mosaic": key, "source": source, "zoom": [zmin, zmax],
              "attribution": attribution, "licence_checked": checked}
    if is_up_to_date(dirs, OUT_NAME, params, args.force):
        return
    licence = mosaic["licence"]
    if not checked:
        warn(sources.SATELLITE_UNVERIFIED)
        licence = f"{licence}. {sources.SATELLITE_UNVERIFIED}"

    mb, work, detail = _tiles(args, dirs, key, source, src, bbox, zmin, zmax)
    mbtiles_set_metadata(mb, {"attribution": attribution, "description": mosaic["name"]})
    tmp = dirs.out / f".tmp-{OUT_NAME}"
    mbtiles_to_pmtiles(mb, tmp)
    install(tmp, dirs, OUT_NAME)
    h = pmtiles_header(dirs.out / OUT_NAME)
    write_meta(dirs, OUT_NAME, params, {
        "kind": "raster-tiles",
        "source": f"{mosaic['name']}: {source}",
        "licence": licence,
        "attribution": attribution,
        "licence_verified_by_operator": checked,
        "bbox": bbox.as_list(),
        "zoom": [h.get("minzoom", zmin), h.get("maxzoom", zmax)],
        "resolution": f"z{zmax} = {zoom_resolution(zmax):.1f} m/px (EPSG:3857 units)",
        **detail,
    })
    if src is not None:
        shutil.rmtree(work, ignore_errors=True)
