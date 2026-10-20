"""Tests for rfmap_tiles.raster that need the GDAL Python bindings (run inside the image)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from osgeo import gdal
except ImportError:  # pragma: no cover - depends on the environment
    gdal = None

from rfmap_tiles import raster  # noqa: E402
from rfmap_tiles.common import BBox, run  # noqa: E402

ROWS = 3600  # one degree of Copernicus GLO-30 rows; smaller tiles hide the window-dependent VRT reads


@unittest.skipIf(gdal is None, "GDAL Python bindings are not installed")
class FinestGridAlignerTest(unittest.TestCase):
    """Copernicus-like tiles across 50°N: 3600 columns per degree below it, 2400 above it.

    Like the real tiles, each origin sits half a pixel west and north of the whole degree,
    so the two zones' grids are offset from each other by a fraction of a pixel.
    """

    @classmethod
    def setUpClass(cls) -> None:
        """Writes the four test tiles once for all tests."""
        cls._tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls._tmp.name)
        cls.rng = np.random.default_rng(7)
        cls.tiles = {(lat, lon): cls._tile(lat, lon) for lat in (49, 50) for lon in (14, 15)}

    @classmethod
    def tearDownClass(cls) -> None:
        """Removes the test tiles."""
        cls._tmp.cleanup()

    @classmethod
    def _tile(cls, lat: int, lon: int) -> Path:
        """One random tile whose north-west corner is at lat + 1, lon, with Copernicus' half-pixel origin."""
        cols = ROWS if lat < 50 else ROWS * 2 // 3
        path = cls.dir / f"t_{lat}_{lon}.tif"
        ds = gdal.GetDriverByName("GTiff").Create(str(path), cols, ROWS, 1, gdal.GDT_Float32)
        ds.SetGeoTransform((lon - 0.5 / cols, 1 / cols, 0, lat + 1 + 0.5 / ROWS, 0, -1 / ROWS))
        ds.SetProjection("EPSG:4326")
        ds.GetRasterBand(1).WriteArray(cls.rng.random((ROWS, cols), dtype=np.float32) * 1000)
        ds = None
        return path

    def _mosaic(self) -> Path:
        """The aligned tiles as one VRT, built the way the DEM step builds it."""
        files = raster.FinestGridAligner(list(self.tiles.values()), self.dir).align()
        vrt = self.dir / "mosaic.vrt"
        run(["gdalbuildvrt", "-q", str(vrt), *map(str, files)])
        return vrt

    def test_fine_tiles_are_used_unchanged(self) -> None:
        """Tiles already on the finest grid are passed through, coarse ones are replaced."""
        files = raster.FinestGridAligner(list(self.tiles.values()), self.dir).align()
        self.assertIn(self.tiles[(49, 14)], files)
        self.assertNotIn(self.tiles[(50, 14)], files)

    def test_inputs_finest_on_different_axes_share_the_finest_grid(self) -> None:
        """One input finer in longitude and another finer in latitude both land on the combined grid."""
        wide = self.dir / "wide.tif"
        tall = self.dir / "tall.tif"
        for path, (cols, rows, lon) in ((wide, (4, 2, 14)), (tall, (2, 4, 15))):
            ds = gdal.GetDriverByName("GTiff").Create(str(path), cols, rows, 1, gdal.GDT_Float32)
            ds.SetGeoTransform((lon, 1 / cols, 0, 50, 0, -1 / rows))
            ds.GetRasterBand(1).WriteArray(np.arange(cols * rows, dtype=np.float32).reshape(rows, cols))
            ds = None
        aligned = raster.FinestGridAligner([wide, tall], self.dir).align()
        for path, source in zip(aligned, (wide, tall), strict=True):
            ds = gdal.Open(str(path))
            self.assertEqual((ds.RasterXSize, ds.RasterYSize), (4, 4))
            original = gdal.Open(str(source)).ReadAsArray()
            reps = (4 // original.shape[0], 4 // original.shape[1])
            np.testing.assert_array_equal(ds.ReadAsArray(), np.kron(original, np.ones(reps, dtype=np.float32)))

    def test_mosaic_keeps_fine_pixels_and_duplicates_coarse_ones(self) -> None:
        """Fine pixels are copied as they are; coarse pixels are duplicated nearest-neighbour."""
        mosaic = gdal.Open(str(self._mosaic())).ReadAsArray()
        fine = gdal.Open(str(self.tiles[(49, 15)])).ReadAsArray()
        coarse = gdal.Open(str(self.tiles[(50, 14)])).ReadAsArray()
        np.testing.assert_array_equal(mosaic[ROWS:, ROWS:], fine)
        # Fine pixel k has its centre at (2k + 1.5) / 3 coarse pixels from the coarse origin.
        nearest = (2 * np.arange(ROWS) + 1.5) // 3
        np.testing.assert_array_equal(mosaic[:ROWS, :ROWS], coarse[:, nearest.astype(int)])

    def test_cog_of_mixed_mosaic_verifies(self) -> None:
        """A COG written from the mixed mosaic matches it pixel for pixel (failed before the fix)."""
        vrt = self._mosaic()
        srcwin = raster.srcwin_for_bbox(vrt, BBox(14.07, 49.0, 15.9, 50.9))
        cog = self.dir / "dem.cog.tif"
        raster.write_cog(vrt, cog, srcwin=srcwin, predictor="YES", overview_resampling="AVERAGE",
                         nodata=-32768.0, out_type="Float32")
        raster.assert_identical(vrt, srcwin, cog, out_dtype="float32")


if __name__ == "__main__":
    unittest.main()
