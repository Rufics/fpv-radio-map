import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import dataset


class FakeRunner:
    """Records the commands and writes the two rasters the way tiles-builder would."""

    def __init__(self, returncode=0):
        self.commands = []
        self.returncode = returncode

    def __call__(self, command, check, timeout):
        self.commands.append(command)
        mount = command[command.index("-v") + 1].rsplit(":", 1)[0]
        out = Path(mount) / "out"
        out.mkdir(parents=True, exist_ok=True)
        step = command[command.index(dataset.DEFAULT_IMAGE) + 1]
        name = "dem.cog.tif" if step == "dem" else "landcover.cog.tif"
        (out / name).write_bytes(step.encode())
        return subprocess.CompletedProcess(command, self.returncode)


class RegionTest(unittest.TestCase):
    def test_bounding_box_is_centred_and_sized(self):
        box = dataset.Region(52.23, 21.01, 25.0, 25.0).bounding_box()
        self.assertAlmostEqual((box.min_lat + box.max_lat) / 2, 52.23)
        self.assertAlmostEqual((box.min_lon + box.max_lon) / 2, 21.01)
        self.assertAlmostEqual((box.max_lat - box.min_lat) * dataset.KM_PER_DEGREE_LAT, 25.0)
        self.assertGreater(box.max_lon - box.min_lon, box.max_lat - box.min_lat)

    def test_default_region_is_valid(self):
        dataset.Region(*dataset.DEFAULT_CENTER, 25.0, 25.0).validate()

    def test_rejects_zone_border(self):
        region = dataset.Region(49.95, 20.0, 25.0, 25.0)
        with self.assertRaisesRegex(dataset.DatasetError, "latitude 50"):
            region.validate()

    def test_rejects_size_out_of_range(self):
        region = dataset.Region(52.23, 21.01, 0.5, 10.0)
        with self.assertRaisesRegex(dataset.DatasetError, "size"):
            region.validate()

    def test_rejects_antimeridian(self):
        region = dataset.Region(10.0, 179.9, 50.0, 50.0)
        with self.assertRaisesRegex(dataset.DatasetError, "180th"):
            region.validate()

    def test_rejects_box_beyond_max_latitude(self):
        region = dataset.Region(84.99, 0.0, 10.0, 10.0)
        with self.assertRaisesRegex(dataset.DatasetError, "85"):
            region.validate()

    def test_rejects_centre_out_of_range(self):
        region = dataset.Region(89.0, 0.0, 10.0, 10.0)
        with self.assertRaisesRegex(dataset.DatasetError, "out of range"):
            region.validate()


class RegionParserTest(unittest.TestCase):
    def test_size_square_and_rectangle(self):
        self.assertEqual(dataset.RegionParser.size("20"), (20.0, 20.0))
        self.assertEqual(dataset.RegionParser.size("30x10"), (30.0, 10.0))

    def test_bad_values(self):
        for parse, text in ((dataset.RegionParser.size, "1x2x3"), (dataset.RegionParser.size, "ax2"),
                            (dataset.RegionParser.center, "52"), (dataset.RegionParser.center, "a,b")):
            with self.subTest(text=text), self.assertRaises(dataset.argparse.ArgumentTypeError):
                parse(text)


class TilesBuilderTest(unittest.TestCase):
    def test_commands_skip_pictures_and_pass_bbox(self):
        builder = dataset.TilesBuilder("rfmap/tiles-builder:1.0", Path("data"), runner=FakeRunner())
        dem, landcover = builder.commands(dataset.BoundingBox(1, 2, 3, 4), force=True)
        self.assertIn("--no-hillshade", dem)
        self.assertEqual(dem[dem.index("--bbox") + 1], "1.00000,2.00000,3.00000,4.00000")
        self.assertEqual(landcover[-1], "--force")
        self.assertIn(f"{Path('data')}:/data", dem)

    def test_rejects_colon_in_directory(self):
        with self.assertRaisesRegex(dataset.DatasetError, "':'"):
            dataset.TilesBuilder(dataset.DEFAULT_IMAGE, Path("data:x"))

    def test_rejects_bad_image_name(self):
        with self.assertRaises(dataset.DatasetError):
            dataset.TilesBuilder("x; rm -rf /", Path("data"))

    def test_failure_and_timeout(self):
        def timeout(command, check, timeout):
            raise subprocess.TimeoutExpired(command, timeout)

        box = dataset.BoundingBox(1, 2, 3, 4)
        with mock.patch.object(dataset.shutil, "which", return_value="/usr/bin/docker"):
            for runner in (FakeRunner(returncode=3), timeout):
                with self.subTest(runner=runner), tempfile.TemporaryDirectory() as tmp:
                    builder = dataset.TilesBuilder(dataset.DEFAULT_IMAGE, Path(tmp), runner=runner)
                    with self.assertRaises(dataset.DatasetError):
                        builder.build(box, False)

    @mock.patch.object(dataset.shutil, "which", return_value=None)
    def test_missing_docker(self, _which):
        builder = dataset.TilesBuilder(dataset.DEFAULT_IMAGE, Path("data"), runner=FakeRunner())
        box = dataset.BoundingBox(1, 2, 3, 4)
        with self.assertRaisesRegex(dataset.DatasetError, "docker"):
            builder.build(box, False)


class CliTest(unittest.TestCase):
    def setUp(self):
        self.runner = FakeRunner()
        self.output = io.StringIO()
        self.cli = dataset.DatasetCli(builder_factory=self._builder, output=self.output)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.which = mock.patch.object(dataset.shutil, "which", return_value="/usr/bin/docker")
        self.which.start()
        self.addCleanup(self.which.stop)

    def _builder(self, image, data_dir):
        return dataset.TilesBuilder(image, data_dir, runner=self.runner)

    def test_bbox_prints_default_region(self):
        self.assertEqual(self.cli.main(["bbox"]), 0)
        self.assertEqual(self.output.getvalue().strip(), "20.82667,52.11771,21.19333,52.34229")

    def test_build_then_verify(self):
        self.assertEqual(self.cli.main(["build", "--out", self.tmp.name, "--size-km", "10"]), 0)
        self.assertEqual(len(self.runner.commands), 2)
        manifest = json.loads((Path(self.tmp.name) / "out" / dataset.MANIFEST_NAME).read_text())
        self.assertEqual(manifest["region"]["size_km"], [10.0, 10.0])
        self.assertEqual(self.cli.main(["verify", "--out", self.tmp.name]), 0)

    def test_verify_detects_change(self):
        self.cli.main(["build", "--out", self.tmp.name])
        (Path(self.tmp.name) / "out" / "dem.cog.tif").write_bytes(b"other")
        self.assertEqual(self.cli.main(["verify", "--out", self.tmp.name]), 1)

    def test_verify_without_build(self):
        self.assertEqual(self.cli.main(["verify", "--out", self.tmp.name]), 1)

    def test_verify_bad_manifest(self):
        out = Path(self.tmp.name) / "out"
        out.mkdir()
        for content in ("{", '{"format": 99}', '{"format": 1, "files": {}}', '{"format": 1}'):
            with self.subTest(content=content):
                (out / dataset.MANIFEST_NAME).write_text(content)
                self.assertEqual(self.cli.main(["verify", "--out", self.tmp.name]), 1)

    def test_build_refuses_invalid_region(self):
        self.assertEqual(self.cli.main(["build", "--out", self.tmp.name, "--center", "49.95,20"]), 1)
        self.assertEqual(self.runner.commands, [])


if __name__ == "__main__":
    unittest.main()
