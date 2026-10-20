"""landcover: ESA WorldCover 10 m -> COG for the clutter module.

WorldCover tiles are 3x3 degree COGs. For each tile only the pixel window
covering the bbox is read (GDAL /vsicurl/ range requests) and kept in the cache,
so a failed download is retried and a rerun resumes. The cached windows are
mosaicked with a VRT and cut with an integer pixel window: class values are
copied unchanged, never resampled.
"""
from __future__ import annotations

import math
import shutil
import time
from pathlib import Path

from . import raster, sources
from .common import BBox, BuildError, Dirs, fresh_dir, http_status, install, is_up_to_date, log, run, warn, write_meta

OUT_NAME = "landcover.cog.tif"
# GDAL retries throttled or failed range requests itself; WindowCache retries the whole window on top.
HTTP_ENV = {"GDAL_DISABLE_READDIR_ON_OPEN": "EMPTY_DIR", "GDAL_HTTP_MAX_RETRY": "10",
            "GDAL_HTTP_RETRY_DELAY": "5", "GDAL_HTTP_TIMEOUT": "120", "CPL_VSIL_CURL_ALLOWED_EXTENSIONS": ".tif"}


def add_arguments(p) -> None:
    g = p.add_argument_group("landcover (ESA WorldCover)")
    g.add_argument("--worldcover-year", choices=sorted(sources.WORLDCOVER_VERSIONS), default="2021",
                   help="2021 = v200 (default), 2020 = v100")


def _tile_name(lat: int, lon: int) -> str:
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


class WindowCache:
    """Copies the bbox window of each remote WorldCover tile into the cache, 1:1 and resumable."""

    ATTEMPTS = 4
    BACKOFF_S = 15

    def __init__(self, bbox: BBox, cache: Path) -> None:
        """Windows for bbox are kept under cache, one file per tile and window."""
        self._bbox = bbox
        self._cache = cache
        cache.mkdir(parents=True, exist_ok=True)

    def fetch(self, url: str) -> Path:
        """The cached window of one tile; downloaded first if a previous run did not finish it."""
        stem = url.rsplit("/", 1)[1].removesuffix(".tif")
        # Named by the exact bbox, not the pixel window, so a cache hit needs no network access.
        key = "_".join(repr(v) for v in self._bbox.as_list())
        dest = self._cache / f"{stem}_{key}.tif"
        if dest.exists():
            log(f"cached: {dest.name}")
            return dest
        part = dest.with_suffix(".part.tif")
        self._copy_with_retries(self._remote(url), part)
        part.replace(dest)
        return dest

    @staticmethod
    def _remote(url: str) -> str:
        """The GDAL path that reads url with HTTP range requests."""
        return f"/vsicurl/{url}"

    def _copy_with_retries(self, remote: str, part: Path) -> None:
        for attempt in range(1, self.ATTEMPTS + 1):
            try:
                self._copy(remote, part)
                return
            except BuildError:
                part.unlink(missing_ok=True)
                if attempt == self.ATTEMPTS:
                    raise
                delay = self.BACKOFF_S * attempt
                warn(f"WorldCover download failed (attempt {attempt} of {self.ATTEMPTS}); retrying in {delay} s")
                time.sleep(delay)

    def _copy(self, remote: str, part: Path) -> None:
        """One attempt: reads the tile header for the pixel window, then copies the window."""
        try:
            with raster.gdal().config_options(HTTP_ENV):
                srcwin = raster.srcwin_for_bbox(remote, self._bbox)
        except RuntimeError as e:
            raise BuildError(f"cannot open {remote}: {e}") from e
        run(["gdal_translate", "-of", "GTiff", *raster.tiff_option_args(),
             "-srcwin", *map(str, srcwin), remote, str(part)], env=HTTP_ENV)


def build(args, dirs: Dirs) -> None:
    if not args.bbox:
        raise BuildError("landcover needs --bbox")
    bbox = BBox.parse(args.bbox)
    # The year and version come from the table, not from the command line.
    year, version = next((y, v) for y, v in sources.WORLDCOVER_VERSIONS.items() if y == args.worldcover_year)
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

    cache = WindowCache(bbox, dirs.cache / f"esa-worldcover-{version}-{year}")
    windows = [cache.fetch(u) for u in urls]
    work = fresh_dir(dirs.work / "landcover")
    vrt = work / "mosaic.vrt"
    run(["gdalbuildvrt", "-vrtnodata", "0", str(vrt), *map(str, windows)])
    srcwin = raster.srcwin_for_bbox(vrt, bbox)
    tmp = dirs.out / f".tmp-{OUT_NAME}"
    attribution = sources.WORLDCOVER["attribution"].format(year=year)
    raster.write_cog(vrt, tmp, srcwin=srcwin, predictor="NO", overview_resampling="MODE", nodata=0,
                     metadata={"RFMAP_SOURCE": f"ESA WorldCover 10m {year} {version}",
                               "RFMAP_ATTRIBUTION": attribution, "RFMAP_LICENCE": "CC BY 4.0"})
    errors = raster.validate_cog(tmp)
    if errors:
        raise BuildError("COG validation failed: " + "; ".join(errors))
    raster.assert_identical(vrt, srcwin, tmp)
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
        "verified_identical_to_source": True,
    })
    shutil.rmtree(work, ignore_errors=True)
