"""Raster helpers shared by dem / landcover / satellite (GDAL Python bindings + CLI)."""
from __future__ import annotations

import json
import math
from pathlib import Path

from .common import BBox, BuildError, log, run

# GeoTIFF creation options of every intermediate (non-COG) raster: tiled, lossless, > 4 GiB safe.
COMPRESS_DEFLATE = "COMPRESS=DEFLATE"
BIGTIFF_IF_SAFER = "BIGTIFF=IF_SAFER"
TIFF_OPTIONS = ("TILED=YES", COMPRESS_DEFLATE, BIGTIFF_IF_SAFER)


def tiff_option_args() -> list[str]:
    """TIFF_OPTIONS as GDAL command-line arguments (-co NAME=VALUE ...)."""
    return [arg for option in TIFF_OPTIONS for arg in ("-co", option)]


def gdal():
    from osgeo import gdal as _gdal  # imported lazily so --help works without GDAL
    _gdal.UseExceptions()
    return _gdal


def info(path: str | Path) -> dict:
    return json.loads(run(["gdalinfo", "-json", str(path)], capture=True))


def pixel_size(path: str | Path) -> tuple[float, float]:
    ds = gdal().Open(str(path))
    gt = ds.GetGeoTransform()
    return gt[1], gt[5]


def srcwin_for_bbox(path: str | Path, bbox: BBox) -> tuple[int, int, int, int]:
    """Integer pixel window (xoff, yoff, xsize, ysize) covering bbox completely.

    Using an integer -srcwin (instead of -projwin) guarantees that gdal_translate
    copies pixels 1:1 without any resampling.
    """
    ds = gdal().Open(str(path))
    x0, dx, rx, y0, ry, dy = ds.GetGeoTransform()
    if rx or ry:
        raise BuildError(f"{path}: rotated rasters are not supported")
    eps = 1e-9
    c0 = math.floor((bbox.min_lon - x0) / dx + eps)
    c1 = math.ceil((bbox.max_lon - x0) / dx - eps)
    r0 = math.floor((bbox.max_lat - y0) / dy + eps)
    r1 = math.ceil((bbox.min_lat - y0) / dy - eps)
    c0, r0 = max(c0, 0), max(r0, 0)
    c1, r1 = min(c1, ds.RasterXSize), min(r1, ds.RasterYSize)
    if c1 <= c0 or r1 <= r0:
        raise BuildError(f"bbox {bbox} does not intersect {path}")
    return c0, r0, c1 - c0, r1 - r0


class FinestGridAligner:
    """Puts tiles of different pixel size onto the grid of the finest tile.

    A VRT that mixes pixel sizes resamples on every read, and its nearest-neighbour
    result depends on the read window, so the written COG cannot be verified against
    it. Coarser tiles are therefore warped once, nearest-neighbour, onto the finest
    tile's grid (origin included); tiles already on that grid are used unchanged.
    """

    def __init__(self, files: list[Path], work: Path) -> None:
        """Reads the grid of every file; warped copies are written into work."""
        if not files:
            raise BuildError("no rasters to align")
        self._files = files
        self._work = work
        self._grids = {f: self._grid(f) for f in files}
        self._ref = self._finest_grid()

    def _finest_grid(self) -> tuple[float, float, float, float, float, float]:
        """The finest spacing on each axis, anchored on a tile that has it.

        Usually one tile is finest on both axes and this is its grid. When one input is
        finer in longitude and another in latitude, the two axes come from different
        tiles, so every input is duplicated onto the combined grid and none loses detail.
        """
        grids = list(self._grids.values())
        x_ref = min(grids, key=lambda g: abs(g[1]))
        y_ref = min(grids, key=lambda g: abs(g[4]))
        return x_ref[0], x_ref[1], x_ref[2], y_ref[3], y_ref[4], y_ref[5]

    @staticmethod
    def _grid(path: Path) -> tuple[float, float, float, float, float, float]:
        """(x0, dx, columns, y0, dy, rows) of a north-up raster."""
        ds = gdal().Open(str(path))
        gt = ds.GetGeoTransform()
        if gt[2] or gt[4]:
            raise BuildError(f"{path.name}: rotated rasters are not supported")
        return gt[0], gt[1], ds.RasterXSize, gt[3], gt[5], ds.RasterYSize

    def align(self) -> list[Path]:
        """The inputs in order, each either unchanged or replaced by its copy on the finest grid."""
        return [f if self._on_reference_grid(f) else self._warp(f, i) for i, f in enumerate(self._files)]

    def _on_reference_grid(self, path: Path) -> bool:
        """True if the file has the reference pixel size and its origin lies on a reference grid line."""
        x0, dx, _, y0, dy, _ = self._grids[path]
        rx0, rdx, _, ry0, rdy, _ = self._ref
        same_size = math.isclose(dx, rdx, rel_tol=1e-9) and math.isclose(dy, rdy, rel_tol=1e-9)
        return same_size and self._is_whole(x0, rx0, rdx) and self._is_whole(y0, ry0, rdy)

    @staticmethod
    def _is_whole(value: float, origin: float, step: float) -> bool:
        """True if value lies a whole number of steps from origin."""
        offset = (value - origin) / step
        return abs(offset - round(offset)) < 1e-6

    @staticmethod
    def _snap(value: float, origin: float, step: float) -> tuple[int, float]:
        """Nearest reference grid line: (index, coordinate). Shared tile edges snap alike."""
        index = round((value - origin) / step)
        return index, origin + index * step

    def _warp(self, path: Path, index: int) -> Path:
        """Copies one tile, nearest-neighbour, onto the reference grid over its own snapped extent."""
        x0, dx, nx, y0, dy, ny = self._grids[path]
        rx0, rdx, _, ry0, rdy, _ = self._ref
        c0, west = self._snap(x0, rx0, rdx)
        c1, east = self._snap(x0 + dx * nx, rx0, rdx)
        r0, north = self._snap(y0, ry0, rdy)
        r1, south = self._snap(y0 + dy * ny, ry0, rdy)
        out = self._work / f"aligned-{index:04d}-{path.stem}.tif"  # --dem-file names may repeat
        run(["gdalwarp", "-overwrite", "-r", "near", "-et", "0", *tiff_option_args(),
             "-te", repr(west), repr(south), repr(east), repr(north),
             "-ts", str(c1 - c0), str(r1 - r0), str(path), str(out)])
        return out


def write_cog(src: str | Path, dst: Path, *, srcwin: tuple[int, int, int, int] | None,
              predictor: str, overview_resampling: str, nodata: float | None = None,
              out_type: str | None = None, metadata: dict | None = None, threads: str = "ALL_CPUS") -> None:
    """gdal_translate to a lossless COG: 512 px internal tiles, DEFLATE, overviews."""
    cmd = ["gdal_translate", "-of", "COG",
           "-co", "BLOCKSIZE=512",
           "-co", COMPRESS_DEFLATE,
           "-co", "LEVEL=6",
           "-co", f"PREDICTOR={predictor}",
           "-co", "OVERVIEWS=IGNORE_EXISTING",
           "-co", f"OVERVIEW_RESAMPLING={overview_resampling}",
           "-co", BIGTIFF_IF_SAFER,
           "-co", f"NUM_THREADS={threads}"]
    if srcwin:
        cmd += ["-srcwin", *map(str, srcwin)]
    if nodata is not None:
        cmd += ["-a_nodata", repr(nodata)]
    if out_type:
        cmd += ["-ot", out_type]
    for k, v in (metadata or {}).items():
        cmd += ["-mo", f"{k}={v}"]
    cmd += [str(src), str(dst)]
    run(cmd, env={"GDAL_NUM_THREADS": "ALL_CPUS"})


def validate_cog(path: Path) -> list[str]:
    """Run GDAL's own COG validator; returns its errors (empty list = valid COG)."""
    gdal()  # enable exceptions before the validator imports osgeo.gdal
    from osgeo_utils.samples import validate_cloud_optimized_geotiff as v
    warnings, errors, _details = v.validate(str(path), full_check=True)
    for w in warnings:
        log(f"COG validator warning: {w}")
    return errors


def assert_identical(window_src: str | Path, srcwin: tuple[int, int, int, int], cog: Path,
                     out_dtype: str | None = None) -> None:
    """Compare every pixel of the COG with the source window (block-wise)."""
    import numpy as np
    g = gdal()
    a_ds, b_ds = g.Open(str(window_src)), g.Open(str(cog))
    xoff, yoff, xs, ys = srcwin
    if (b_ds.RasterXSize, b_ds.RasterYSize) != (xs, ys):
        raise BuildError("COG size differs from the source window")
    a, b = a_ds.GetRasterBand(1), b_ds.GetRasterBand(1)
    step = 2048
    for r in range(0, ys, step):
        h = min(step, ys - r)
        aa = a.ReadAsArray(xoff, yoff + r, xs, h)
        bb = b.ReadAsArray(0, r, xs, h)
        if out_dtype:
            aa = aa.astype(out_dtype)
        if not np.array_equal(aa, bb, equal_nan=aa.dtype.kind == "f"):
            raise BuildError(f"pixel values in {cog.name} differ from the source (rows {r}-{r + h}); "
                             "this must never happen — please report")
    log(f"verified: all {xs}x{ys} pixels of {cog.name} are identical to the source data")


def raster_summary(path: Path) -> dict:
    i = info(path)
    gt = i["geoTransform"]
    w, h = i["size"]
    b = i["bands"][0]
    return {
        "bbox": [gt[0], gt[3] + gt[5] * h, gt[0] + gt[1] * w, gt[3]],
        "size_px": [w, h],
        "pixel_size_deg": [gt[1], abs(gt[5])],
        "crs": "EPSG:4326" if "4326" in json.dumps(i.get("coordinateSystem", {})) else "see file",
        "data_type": b["type"],
        "nodata": b.get("noDataValue"),
        "block": b.get("block"),
        "overviews": len(b.get("overviews", [])),
        "layout": i.get("metadata", {}).get("IMAGE_STRUCTURE", {}).get("LAYOUT"),
        "compression": i.get("metadata", {}).get("IMAGE_STRUCTURE", {}).get("COMPRESSION"),
    }
