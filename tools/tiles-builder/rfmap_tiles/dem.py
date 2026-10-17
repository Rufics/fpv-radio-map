"""dem: elevation tiles -> calculation COG, plus display-only hillshade and contours.

The calculation DEM (out/dem.cog.tif) keeps the source grid and the source
values bit-for-bit: tiles are mosaicked with a VRT and cut with an integer
pixel window, never warped or resampled. The server's radio calculations read
this COG directly. hillshade.pmtiles and contours.pmtiles are derived from the
same COG and are for display only.
"""
from __future__ import annotations

import gzip
import math
import shutil
from pathlib import Path

from . import raster, sources
from .common import (BBox, BuildError, Dirs, download, fresh_dir, http_status, install, is_up_to_date,
                     log, mbtiles_set_metadata, mbtiles_to_pmtiles, overview_factors, pmtiles_header,
                     read_meta, run, tile_aligned_extent, warn, write_meta, zoom_resolution)

DEM_OUT = "dem.cog.tif"
HILLSHADE_OUT = "hillshade.pmtiles"
CONTOURS_OUT = "contours.pmtiles"
NODATA = -32768.0


def add_arguments(p) -> None:
    g = p.add_argument_group("dem (elevation)")
    g.add_argument("--dem-source", choices=["copernicus", "srtm", "file"], default="copernicus",
                   help="copernicus = Copernicus DEM GLO-30 from AWS (default); srtm = AWS Terrain Tiles "
                        "1\" HGT (fallback); file = local rasters given with --dem-file")
    g.add_argument("--dem-file", action="append", default=[],
                   help="local DEM raster(s) on one shared grid (repeatable); implies --dem-source file")
    g.add_argument("--allow-dem-resample", action="store_true",
                   help="allow mosaicking tiles of different pixel size (e.g. Copernicus across 50°N) by "
                        "nearest-neighbour duplication onto the finest grid. Off by default.")
    g.add_argument("--contour-interval", type=float, default=20.0, help="contour interval in metres (default 20)")
    g.add_argument("--contour-index-every", type=int, default=5,
                   help="every Nth contour is an index contour, shown from lower zooms (default 5 -> 100 m)")
    g.add_argument("--contours-minzoom", type=int, default=10)
    g.add_argument("--contours-maxzoom", type=int, default=14)
    g.add_argument("--hillshade-minzoom", type=int, default=6)
    g.add_argument("--hillshade-maxzoom", type=int, default=12,
                   help="default 12 (~25 m/px at 48°N, matching the 30 m DEM)")
    g.add_argument("--hillshade-style", choices=["alpha", "gray"], default="alpha",
                   help="alpha = transparent PNG (black shadows / faint white highlights) for dark UIs; "
                        "gray = classic grey hillshade")
    g.add_argument("--hillshade-exaggeration", type=float, default=1.0, help="vertical exaggeration (-z)")
    g.add_argument("--no-hillshade", action="store_true")
    g.add_argument("--no-contours", action="store_true")


# ----------------------------------------------------------------------------- tile lists

def _lat_lon_tiles(bbox: BBox, step: int = 1) -> list[tuple[int, int]]:
    lat0 = math.floor(bbox.min_lat / step) * step
    lon0 = math.floor(bbox.min_lon / step) * step
    lat1 = math.ceil(bbox.max_lat / step) * step
    lon1 = math.ceil(bbox.max_lon / step) * step
    return [(la, lo) for la in range(lat0, lat1, step) for lo in range(lon0, lon1, step)]


def _cop_name(lat: int, lon: int) -> tuple[str, str]:
    return (f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}", f"{'E' if lon >= 0 else 'W'}{abs(lon):03d}")


def _fetch_copernicus(bbox: BBox, dirs: Dirs) -> tuple[list[Path], list[str], list[str]]:
    files, missing, urls = [], [], []
    cache = dirs.cache / "copernicus-dem-30m"
    for lat, lon in _lat_lon_tiles(bbox):
        la, lo = _cop_name(lat, lon)
        url = sources.COPERNICUS_DEM_URL.format(lat=la, lon=lo)
        dest = cache / url.rsplit("/", 1)[1]
        if not dest.exists():
            st = http_status(url)
            if st in (403, 404):
                warn(f"Copernicus tile {la}{lo} does not exist (ocean, or withheld from the public "
                     "release); that area will be no-data")
                missing.append(f"{la}{lo}")
                continue
        download(url, dest)
        files.append(dest)
        urls.append(url)
    return files, missing, urls


def _fetch_srtm(bbox: BBox, dirs: Dirs) -> tuple[list[Path], list[str], list[str]]:
    files, missing, urls = [], [], []
    cache = dirs.cache / "srtm-skadi"
    for lat, lon in _lat_lon_tiles(bbox):
        la, lo = _cop_name(lat, lon)
        url = sources.SRTM_SKADI_URL.format(lat=la, lon=lo)
        hgt = cache / f"{la}{lo}.hgt"
        if not hgt.exists():
            if http_status(url) in (403, 404):
                warn(f"SRTM tile {la}{lo} does not exist; that area will be no-data")
                missing.append(f"{la}{lo}")
                continue
            gz = download(url, cache / f"{la}{lo}.hgt.gz")
            with gzip.open(gz, "rb") as fi, open(hgt.with_suffix(".part"), "wb") as fo:
                shutil.copyfileobj(fi, fo)
            hgt.with_suffix(".part").rename(hgt)
            gz.unlink()
        files.append(hgt)
        urls.append(url)
    return files, missing, urls


# ----------------------------------------------------------------------------- build

def build(args, dirs: Dirs) -> None:
    if not args.bbox:
        raise BuildError("dem needs --bbox minLon,minLat,maxLon,maxLat")
    bbox = BBox.parse(args.bbox)
    source = "file" if args.dem_file else args.dem_source
    if source == "file" and not args.dem_file:
        raise BuildError("--dem-source file needs at least one --dem-file")
    if bbox.area_km2() > 250_000:
        warn(f"bbox is {bbox.area_km2():,.0f} km²; this will need a lot of disk and time")

    params = {"bbox": str(bbox), "source": source,
              "files": sorted(str(f) for f in args.dem_file), "allow_resample": args.allow_dem_resample}
    if not is_up_to_date(dirs, DEM_OUT, params, args.force):
        _build_cog(args, dirs, bbox, source, params)
    dem = dirs.out / DEM_OUT
    dem_meta = read_meta(dirs, DEM_OUT) or {}

    if not args.no_hillshade:
        _build_hillshade(args, dirs, bbox, dem, dem_meta)
    if not args.no_contours:
        _build_contours(args, dirs, bbox, dem, dem_meta)


def _build_cog(args, dirs: Dirs, bbox: BBox, source: str, params: dict) -> None:
    work = fresh_dir(dirs.work / "dem")
    if source == "copernicus":
        files, missing, urls = _fetch_copernicus(bbox, dirs)
        src_info = sources.COPERNICUS_DEM
    elif source == "srtm":
        files, missing, urls = _fetch_srtm(bbox, dirs)
        src_info = sources.SRTM
    else:
        files = [Path(f) for f in args.dem_file]
        for f in files:
            if not f.is_file():
                raise BuildError(f"--dem-file not found: {f}")
        missing, urls = [], [f"file:{f.name}" for f in files]
        src_info = {"source": "operator-supplied DEM files", "licence": "as supplied by the operator",
                    "attribution": "set by the operator", "vertical_datum": "as supplied"}
    if not files:
        raise BuildError("no elevation tiles found for this bbox")

    # Refuse silent resampling: all inputs must share one pixel size.
    sizes = {f: raster.pixel_size(f) for f in files}
    distinct = {(round(dx, 12), round(dy, 12)) for dx, dy in sizes.values()}
    vrt_opts = []
    resampled = False
    if len(distinct) > 1:
        detail = ", ".join(f"{f.name}: {dx:.9f}x{abs(dy):.9f}" for f, (dx, dy) in sizes.items())
        if not args.allow_dem_resample:
            raise BuildError("the DEM tiles for this bbox have different pixel sizes (Copernicus GLO-30 "
                             "widens its longitude spacing above 50° latitude). Mosaicking them would "
                             f"resample. Split the bbox at the zone border, or pass --allow-dem-resample. {detail}")
        warn("mixing pixel sizes: coarser tiles are duplicated nearest-neighbour onto the finest grid")
        vrt_opts = ["-resolution", "highest", "-r", "nearest"]
        resampled = True

    vrt = work / "mosaic.vrt"
    run(["gdalbuildvrt", *vrt_opts, "-vrtnodata", repr(NODATA), str(vrt), *map(str, files)])
    srcwin = raster.srcwin_for_bbox(vrt, bbox)
    tmp = dirs.out / f".tmp-{DEM_OUT}"
    raster.write_cog(vrt, tmp, srcwin=srcwin, predictor="YES", overview_resampling="AVERAGE",
                     nodata=NODATA, out_type="Float32",
                     metadata={"RFMAP_SOURCE": src_info["source"],
                               "RFMAP_ATTRIBUTION": src_info["attribution"],
                               "RFMAP_VERTICAL_DATUM": src_info.get("vertical_datum", ""),
                               "RFMAP_ROLE": "calculation DEM (read directly by the server)"})
    errors = raster.validate_cog(tmp)
    if errors:
        raise BuildError("COG validation failed: " + "; ".join(errors))
    raster.assert_identical(vrt, srcwin, tmp, out_dtype="float32")
    install(tmp, dirs, DEM_OUT)
    summary = raster.raster_summary(dirs.out / DEM_OUT)
    write_meta(dirs, DEM_OUT, params, {
        "kind": "calculation-dem",
        "source": src_info["source"],
        "source_urls": urls,
        "missing_tiles": missing,
        "licence": src_info["licence"],
        "attribution": src_info["attribution"],
        "vertical_datum": src_info.get("vertical_datum"),
        "notes": src_info.get("notes"),
        "requested_bbox": bbox.as_list(),
        "bbox": summary["bbox"],
        "resolution": f"{summary['pixel_size_deg'][0] * 3600:.4g}\" x {summary['pixel_size_deg'][1] * 3600:.4g}\" "
                      f"(source grid, {'NEAREST-DUPLICATED from mixed grids' if resampled else 'not resampled'})",
        "raster": summary,
        "verified_identical_to_source": True,
    })
    shutil.rmtree(work, ignore_errors=True)


def _derived_params(dem_meta: dict, extra: dict) -> dict:
    return {"dem_sha256": dem_meta.get("sha256"), **extra}


def _build_hillshade(args, dirs: Dirs, bbox: BBox, dem: Path, dem_meta: dict) -> None:
    zmin, zmax = args.hillshade_minzoom, args.hillshade_maxzoom
    params = _derived_params(dem_meta, {"zoom": [zmin, zmax], "style": args.hillshade_style,
                                        "z": args.hillshade_exaggeration})
    if is_up_to_date(dirs, HILLSHADE_OUT, params, args.force):
        return
    work = fresh_dir(dirs.work / "hillshade")
    res = zoom_resolution(zmax)
    te = tile_aligned_extent(bbox, zmax)
    merc = work / "dem3857.tif"
    # Display copy only: reprojected to Web Mercator at the tile resolution.
    run(["gdalwarp", "-t_srs", "EPSG:3857", "-r", "bilinear", "-tr", repr(res), repr(res),
         "-te", *map(repr, te), "-srcnodata", repr(NODATA), "-dstnodata", repr(NODATA),
         "-multi", "-wo", "NUM_THREADS=ALL_CPUS",
         "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE", "-co", "BIGTIFF=IF_SAFER", str(dem), str(merc)])
    hs = work / "hillshade.tif"
    # 1 EPSG:3857 unit is cos(lat) metres on the ground at the bbox centre.
    scale = math.cos(math.radians(bbox.center_lat))
    run(["gdaldem", "hillshade", "-multidirectional", "-compute_edges", "-z", repr(args.hillshade_exaggeration),
         "-s", repr(scale), "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE", "-co", "BIGTIFF=IF_SAFER",
         str(merc), str(hs)])
    if args.hillshade_style == "alpha":
        rgba = work / "hillshade-rgba.tif"
        _shade_to_rgba(hs, rgba)
        hs = rgba
    mb = work / "hillshade.mbtiles"
    run(["gdal_translate", "-of", "MBTiles", "-co", "TILE_FORMAT=PNG", "-co", "NAME=hillshade",
         "-co", "DESCRIPTION=Hillshade (display only) derived from dem.cog.tif",
         str(hs), str(mb)])
    if zmax > zmin:
        run(["gdaladdo", "-r", "average", str(mb), *overview_factors(zmin, zmax)])
    mbtiles_set_metadata(mb, {"attribution": dem_meta.get("attribution", "")})
    tmp = dirs.out / f".tmp-{HILLSHADE_OUT}"
    mbtiles_to_pmtiles(mb, tmp)
    install(tmp, dirs, HILLSHADE_OUT)
    h = pmtiles_header(dirs.out / HILLSHADE_OUT)
    write_meta(dirs, HILLSHADE_OUT, params, {
        "kind": "raster-tiles", "role": "display only (calculations read dem.cog.tif)",
        "derived_from": DEM_OUT,
        "source": f"hillshade of {DEM_OUT} ({dem_meta.get('source', '')})",
        "licence": dem_meta.get("licence"), "attribution": dem_meta.get("attribution"),
        "bbox": bbox.as_list(), "zoom": [h.get("minzoom", zmin), h.get("maxzoom", zmax)],
        "resolution": f"z{zmax} = {res:.1f} m/px (EPSG:3857 units), 256 px PNG tiles",
        "hillshade": {"style": args.hillshade_style, "multidirectional": True,
                      "exaggeration": args.hillshade_exaggeration, "scale": scale},
    })
    shutil.rmtree(work, ignore_errors=True)


def _shade_to_rgba(src: Path, dst: Path) -> None:
    """Grey hillshade -> RGBA: black shadows / faint white highlights, flat = transparent."""
    import numpy as np
    g = raster.gdal()
    s_ds = g.Open(str(src))
    drv = g.GetDriverByName("GTiff")
    d_ds = drv.Create(str(dst), s_ds.RasterXSize, s_ds.RasterYSize, 4, g.GDT_Byte,
                      ["TILED=YES", "COMPRESS=DEFLATE", "BIGTIFF=IF_SAFER", "PHOTOMETRIC=RGB", "ALPHA=YES"])
    d_ds.SetGeoTransform(s_ds.GetGeoTransform())
    d_ds.SetProjection(s_ds.GetProjection())
    band = s_ds.GetRasterBand(1)
    flat = 255 * math.sin(math.radians(45))  # value of a flat cell at the default 45° sun altitude
    step = 1024
    for r in range(0, s_ds.RasterYSize, step):
        h = min(step, s_ds.RasterYSize - r)
        v = band.ReadAsArray(0, r, s_ds.RasterXSize, h).astype("float32")
        dark = v < flat
        alpha = np.where(dark, (flat - v) / flat * 210.0, (v - flat) / (255 - flat) * 70.0)
        alpha[v == 0] = 0  # gdaldem nodata
        rgb = np.where(dark, 0, 255).astype("uint8")
        for b in (1, 2, 3):
            d_ds.GetRasterBand(b).WriteArray(rgb, 0, r)
        d_ds.GetRasterBand(4).WriteArray(np.clip(alpha, 0, 255).astype("uint8"), 0, r)
    d_ds.FlushCache()
    d_ds = None


def _build_contours(args, dirs: Dirs, bbox: BBox, dem: Path, dem_meta: dict) -> None:
    interval = args.contour_interval
    if interval <= 0:
        raise BuildError("--contour-interval must be > 0")
    index_every = max(1, args.contour_index_every)
    zmin, zmax = args.contours_minzoom, args.contours_maxzoom
    params = _derived_params(dem_meta, {"interval": interval, "index_every": index_every, "zoom": [zmin, zmax]})
    if is_up_to_date(dirs, CONTOURS_OUT, params, args.force):
        return
    work = fresh_dir(dirs.work / "contours")
    raw = work / "raw.gpkg"
    run(["gdal_contour", "-a", "ele", "-i", repr(interval), "-snodata", repr(NODATA),
         "-f", "GPKG", "-nln", "contour", str(dem), str(raw)])
    idx = interval * index_every
    layers = work / "layers.gpkg"
    run(["ogr2ogr", "-f", "GPKG", str(layers), str(raw), "-nln", "contours", "-dialect", "SQLite", "-sql",
         "SELECT CAST(ROUND(ele) AS INTEGER) AS ele, geom FROM contour"])
    run(["ogr2ogr", "-update", str(layers), str(raw), "-nln", "contours_index", "-dialect", "SQLite", "-sql",
         f"SELECT CAST(ROUND(ele) AS INTEGER) AS ele, geom FROM contour "
         f"WHERE ABS(ele / {idx!r} - ROUND(ele / {idx!r})) < 1e-6"])
    conf = ('{"contours": {"minzoom": %d, "maxzoom": %d}, "contours_index": {"minzoom": %d, "maxzoom": %d}}'
            % (max(zmin, min(zmax, 12)), zmax, zmin, zmax))
    # Written as MBTiles and converted with go-pmtiles, so the PMTiles header zoom range is
    # computed from the tiles that actually exist (GDAL's direct PMTiles writer copies MINZOOM).
    mb = work / "contours.mbtiles"
    run(["ogr2ogr", "-f", "MBTiles", str(mb), str(layers),
         "-dsco", f"MINZOOM={zmin}", "-dsco", f"MAXZOOM={zmax}", "-dsco", f"CONF={conf}",
         "-dsco", "NAME=contours", "-dsco", f"DESCRIPTION=Contours every {interval:g} m (display only)",
         "-dsco", "MAX_SIZE=1000000"])
    mbtiles_set_metadata(mb, {"attribution": dem_meta.get("attribution", "")})
    tmp = dirs.out / f".tmp-{CONTOURS_OUT}"
    mbtiles_to_pmtiles(mb, tmp)
    install(tmp, dirs, CONTOURS_OUT)
    h = pmtiles_header(dirs.out / CONTOURS_OUT)
    write_meta(dirs, CONTOURS_OUT, params, {
        "kind": "vector-tiles", "role": "display only (calculations read dem.cog.tif)",
        "derived_from": DEM_OUT,
        "source": f"contours of {DEM_OUT} ({dem_meta.get('source', '')})",
        "licence": dem_meta.get("licence"), "attribution": dem_meta.get("attribution"),
        "bbox": bbox.as_list(), "zoom": [h.get("minzoom", zmin), h.get("maxzoom", zmax)],
        "resolution": f"interval {interval:g} m, index every {idx:g} m",
        "layers": {"contours": f"all contours (from z{max(zmin, min(zmax, 12))}), attribute ele (m)",
                   "contours_index": f"every {idx:g} m (from z{zmin}), attribute ele (m)"},
    })
    shutil.rmtree(work, ignore_errors=True)
