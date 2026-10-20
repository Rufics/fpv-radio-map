"""Tests for rfmap_tiles.landcover that need the GDAL Python bindings (run inside the image)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from osgeo import gdal
except ImportError:  # pragma: no cover - depends on the environment
    gdal = None

from rfmap_tiles import landcover  # noqa: E402
from rfmap_tiles.common import BBox, BuildError, run  # noqa: E402

PIXELS = 1200  # 3 degrees at 1/400 degree


class LocalWindowCache(landcover.WindowCache):
    """Reads local files instead of URLs and does not wait between attempts."""

    BACKOFF_S = 0

    @staticmethod
    def _remote(url: str) -> str:
        """The local path itself."""
        return url


@unittest.skipIf(gdal is None, "GDAL Python bindings are not installed")
class WindowCacheTest(unittest.TestCase):
    """A WorldCover-like 3x3 degree tile with random class values."""

    def setUp(self) -> None:
        """Writes the tile and an empty cache directory."""
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.tile = self.dir / "ESA_WorldCover_10m_2021_v200_N51E018_Map.tif"
        ds = gdal.GetDriverByName("GTiff").Create(str(self.tile), PIXELS, PIXELS, 1, gdal.GDT_Byte)
        ds.SetGeoTransform((18, 3 / PIXELS, 0, 54, 0, -3 / PIXELS))
        ds.SetProjection("EPSG:4326")
        self.values = np.random.default_rng(3).integers(10, 100, (PIXELS, PIXELS), dtype=np.uint8)
        ds.GetRasterBand(1).WriteArray(self.values)
        ds = None
        self.bbox = BBox.parse("18.5,52.25,19,52.5")
        self.cache = LocalWindowCache(self.bbox, self.dir / "cache")

    def tearDown(self) -> None:
        """Removes the test files."""
        self._tmp.cleanup()

    def test_window_is_copied_unchanged(self) -> None:
        """The cached window holds exactly the source pixels of the bbox."""
        window = gdal.Open(str(self.cache.fetch(str(self.tile))))
        self.assertEqual((window.RasterXSize, window.RasterYSize), (200, 100))
        np.testing.assert_array_equal(window.ReadAsArray(), self.values[600:700, 200:400])

    def test_cached_window_is_reused(self) -> None:
        """A second fetch returns the cached file without opening the source at all."""
        first = self.cache.fetch(str(self.tile))
        with mock.patch.object(landcover, "run") as runner, \
                mock.patch.object(landcover.raster, "srcwin_for_bbox") as header:
            self.assertEqual(self.cache.fetch(str(self.tile)), first)
        runner.assert_not_called()
        header.assert_not_called()

    def test_failed_header_read_is_retried(self) -> None:
        """A failed read of the tile header is retried like a failed copy."""
        real = landcover.raster.srcwin_for_bbox
        with mock.patch.object(landcover.raster, "srcwin_for_bbox",
                               side_effect=[RuntimeError("HTTP error"), real(self.tile, self.bbox)]):
            window = self.cache.fetch(str(self.tile))
        np.testing.assert_array_equal(gdal.Open(str(window)).ReadAsArray(), self.values[600:700, 200:400])

    def test_failed_download_is_retried(self) -> None:
        """A failed copy leaves no partial file and the next attempt completes the window."""
        calls = []

        def flaky(cmd, **kwargs):
            calls.append(cmd)
            if len(calls) == 1:
                Path(cmd[-1]).write_bytes(b"partial")
                raise BuildError("command failed (exit 1): gdal_translate")
            return run(cmd, **kwargs)

        with mock.patch.object(landcover, "run", side_effect=flaky):
            window = self.cache.fetch(str(self.tile))
        self.assertEqual(len(calls), 2)
        self.assertEqual([p.name for p in window.parent.iterdir()], [window.name])
        np.testing.assert_array_equal(gdal.Open(str(window)).ReadAsArray(), self.values[600:700, 200:400])

    def test_gives_up_after_the_last_attempt(self) -> None:
        """When every attempt fails the error reaches the caller and nothing is cached."""
        failing = mock.patch.object(landcover, "run", side_effect=BuildError("command failed"))
        with failing as runner, self.assertRaises(BuildError):
            self.cache.fetch(str(self.tile))
        self.assertEqual(runner.call_count, LocalWindowCache.ATTEMPTS)
        self.assertEqual(list((self.dir / "cache").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
