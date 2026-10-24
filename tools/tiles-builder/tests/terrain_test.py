"""Tests for the Terrarium encoding of terrain.pmtiles (numpy only, no GDAL needed)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import numpy as np
except ImportError:  # pragma: no cover - depends on the environment
    np = None

from rfmap_tiles import dem  # noqa: E402


def decode(r, g, b):
    """MapLibre's Terrarium decoding."""
    return r.astype("float64") * 256 + g + b / 256.0 - 32768


@unittest.skipIf(np is None, "numpy is not installed")
class TerrariumTest(unittest.TestCase):
    def test_round_trips_to_a_256th_of_a_metre(self) -> None:
        heights = np.array([[-431.3, 0.0, 123.456], [622.0, 4807.81, 8848.86]], dtype="float32")
        back = decode(*dem.terrarium_rgb(heights))
        np.testing.assert_allclose(back, heights.astype("float64"), atol=1 / 256 + 1e-6)
        self.assertTrue(np.all(back <= heights.astype("float64") + 1e-9), "the encoding rounds down")

    def test_no_data_is_sea_level(self) -> None:
        r, g, b = dem.terrarium_rgb(np.array([[dem.NODATA, 10.0]], dtype="float32"))
        self.assertEqual(decode(r, g, b)[0, 0], 0.0)
        self.assertEqual(decode(r, g, b)[0, 1], 10.0)

    def test_clamps_to_the_encodable_range(self) -> None:
        r, g, b = dem.terrarium_rgb(np.array([[-40000.0, 40000.0]]))
        self.assertEqual(decode(r, g, b)[0, 0], -32768.0)
        self.assertEqual(int(r[0, 1]), 255)
        self.assertEqual(int(g[0, 1]), 255)


if __name__ == "__main__":
    unittest.main()
