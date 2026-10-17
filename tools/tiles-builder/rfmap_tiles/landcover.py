"""landcover: ESA WorldCover 10 m -> COG for the clutter module.

WorldCover tiles are 3x3 degree COGs. Only the blocks covering the bbox are
read (GDAL /vsicurl/ range requests), mosaicked with a VRT and cut with an
integer pixel window: class values are copied unchanged, never resampled.
"""
from __future__ import annotations

import math
import shutil

from . import raster, sources
from .common import BBox, BuildError, Dirs, fresh_dir, http_status, install, is_up_to_date, log, run, warn, write_meta

OUT_NAME = "landcover.cog.tif"


def add_arguments(p) -> None:
    g = p.add_argument_group("landcover (ESA WorldCover)")
    g.add_argument("--worldcover-year", choices=sorted(sources.WORLDCOVER_VERSIONS), default="2021",
                   help="2021 = v200 (default), 2020 = v100")


def _tile_name(lat: int, lon: int) -> str:
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


def build(args, dirs: Dirs) -> None:
    if not args.bbox:
        raise BuildError("landcover needs --bbox")
    bbox = BBox.parse(args.bbox)
    year = args.worldcover_year
    version = sources.WORLDCOVER_VERSIONS[year]
    params = {"bbox": str(bbox), "year": year, "version": version}
    if is_up_to_date(dirs, OUT_NAME, params, args.force):
        return

    urls, missing = [], []
    for lat in range(math.floor(bbox.min_lat / 3) * 3, math.ceil(bbox.max_lat / 3) * 3, 3):
        for lon in range(math.floor(bbox.min_lon / 3) * 3, math.ceil(bbox.max_lon / 3) * 3, 3):
            tile = _tile_name(lat, lon)
            url = sources.WORLDCOVER_URL.format(version=version, year=year, tile=tile)
            if http_status(url) in (403, 404):
                warn(f"WorldCover tile {tile} does not exist (open ocean); area will be no-data (0)")
                missing.append(tile)
            else:
                urls.append(url)
    if not urls:
        raise BuildError("no WorldCover tiles for this bbox")

    work = fresh_dir(dirs.work / "landcover")
    vrt = work / "mosaic.vrt"
    run(["gdalbuildvrt", "-vrtnodata", "0", str(vrt), *[f"/vsicurl/{u}" for u in urls]],
        env={"GDAL_DISABLE_READDIR_ON_OPEN": "EMPTY_DIR", "GDAL_HTTP_MAX_RETRY": "5",
             "GDAL_HTTP_RETRY_DELAY": "3", "CPL_VSIL_CURL_ALLOWED_EXTENSIONS": ".tif"})
    srcwin = raster.srcwin_for_bbox(vrt, bbox)
    tmp = dirs.out / f".tmp-{OUT_NAME}"
    attribution = sources.WORLDCOVER["attribution"].format(year=year)
    log("reading WorldCover blocks over HTTP (range requests)")
    raster.write_cog(vrt, tmp, srcwin=srcwin, predictor="NO", overview_resampling="MODE", nodata=0,
                     metadata={"RFMAP_SOURCE": f"ESA WorldCover 10m {year} {version}",
                               "RFMAP_ATTRIBUTION": attribution, "RFMAP_LICENCE": "CC BY 4.0"})
    errors = raster.validate_cog(tmp)
    if errors:
        raise BuildError("COG validation failed: " + "; ".join(errors))
    verified = srcwin[2] * srcwin[3] <= 50_000_000  # re-reading huge areas over HTTP is too slow
    if verified:
        raster.assert_identical(vrt, srcwin, tmp)
    else:
        log("area too large for a full pixel re-check over HTTP; skipped (copy is a 1:1 -srcwin)")
    install(tmp, dirs, OUT_NAME)
    summary = raster.raster_summary(dirs.out / OUT_NAME)
    write_meta(dirs, OUT_NAME, params, {
        "kind": "landcover",
        "source": sources.WORLDCOVER["source"].format(year=year, version=version),
        "source_urls": urls, "missing_tiles": missing,
        "licence": sources.WORLDCOVER["licence"],
        "attribution": attribution,
        "classes": sources.WORLDCOVER["classes"],
        "requested_bbox": bbox.as_list(),
        "bbox": summary["bbox"],
        "resolution": f"{summary['pixel_size_deg'][0] * 3600:.4g}\" (~10 m), source grid, not resampled",
        "raster": summary,
        "verified_identical_to_source": verified,
    })
    shutil.rmtree(work, ignore_errors=True)
