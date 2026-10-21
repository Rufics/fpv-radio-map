#!/usr/bin/env python3
"""Builds and checks the fixed terrain dataset the load tests run against.

    python3 -I tools/perf/dataset.py bbox   [--preset NAME | --size-km W[xH]] [--center LAT,LON]
    python3 -I tools/perf/dataset.py build  --out DIR [--preset NAME | --size-km W[xH]] [--center LAT,LON]
    python3 -I tools/perf/dataset.py verify --out DIR

`build` turns a square (or rectangular) region of the given size around a centre point into a
bounding box and runs the rfmap/tiles-builder image for the two rasters the radio calculations
read: dem.cog.tif and landcover.cog.tif. Hillshade and contours are skipped, they are only
pictures. It then writes perf-dataset.json next to them with the region, the bounding box and
the SHA-256 of each raster, so later runs can check with `verify` that they test against exactly
the same terrain. Keep DIR between runs and reuse it; nothing is downloaded again while the
tiles-builder cache in DIR/cache is kept.

The data is real: Copernicus DEM GLO-30 and ESA WorldCover, downloaded by tiles-builder. The
default region is 25 km x 25 km around the centre of Warsaw (52.23 N, 21.01 E): dense urban
clutter next to open land and forest, and 25 km covers typical FPV ranges with margin. A region
must not cross 50, 60, 70 or 80 degrees of latitude, where Copernicus DEM changes its pixel
spacing.

The exit code is 0 on success, 1 when the build or the check fails, 2 on a usage error.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

MANIFEST_NAME = "perf-dataset.json"
MANIFEST_FORMAT = 1
RASTERS = ("dem.cog.tif", "landcover.cog.tif")
DEFAULT_IMAGE = "rfmap/tiles-builder"
DEFAULT_CENTER = (52.23, 21.01)
DEFAULT_PRESET = "medium"
PRESETS_KM = {"small": 10.0, "medium": 25.0, "large": 50.0}
MIN_SIZE_KM = 1.0
MAX_SIZE_KM = 200.0
KM_PER_DEGREE_LAT = 111.32
# tiles-builder rejects a bbox beyond this latitude.
MAX_LATITUDE_DEG = 85.0
# Copernicus DEM widens its longitude spacing at these latitudes; tiles-builder refuses to mosaic
# across them without resampling.
DEM_ZONE_BORDERS_DEG = (50.0, 60.0, 70.0, 80.0)
BUILD_TIMEOUT_SECONDS = 3600
HASH_CHUNK_BYTES = 1 << 20
IMAGE_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._/-]*(:[A-Za-z0-9._-]+)?$")


class DatasetError(Exception):
    """The dataset cannot be built, or does not match its manifest."""


@dataclass(frozen=True)
class BoundingBox:
    """A WGS84 bounding box in degrees."""

    min_lon: float
    min_lat: float
    max_lon: float
    max_lat: float

    def as_argument(self):
        """The minLon,minLat,maxLon,maxLat form tiles-builder takes, rounded to ~1 m."""
        return ",".join(f"{value:.5f}" for value in self.as_list())

    def as_list(self):
        return [self.min_lon, self.min_lat, self.max_lon, self.max_lat]


@dataclass(frozen=True)
class Region:
    """A region of a given size in km around a centre point."""

    center_lat: float
    center_lon: float
    width_km: float
    height_km: float

    def validate(self):
        """Raises DatasetError when tiles-builder could not build this region as it is."""
        for size in (self.width_km, self.height_km):
            if not MIN_SIZE_KM <= size <= MAX_SIZE_KM:
                raise DatasetError(f"region size must be {MIN_SIZE_KM:g} to {MAX_SIZE_KM:g} km, got {size:g}")
        if not -MAX_LATITUDE_DEG < self.center_lat < MAX_LATITUDE_DEG or not -180.0 < self.center_lon < 180.0:
            raise DatasetError(f"centre {self.center_lat},{self.center_lon} is out of range")
        box = self.bounding_box()
        if box.min_lat <= -MAX_LATITUDE_DEG or box.max_lat >= MAX_LATITUDE_DEG:
            raise DatasetError(f"region must stay within {MAX_LATITUDE_DEG:g} degrees of latitude")
        if box.min_lon <= -180.0 or box.max_lon >= 180.0:
            raise DatasetError("regions that cross the 180th meridian are not supported")
        for border in DEM_ZONE_BORDERS_DEG:
            if box.min_lat < border < box.max_lat or box.min_lat < -border < box.max_lat:
                raise DatasetError(f"region crosses latitude {border:g}, where the DEM pixel spacing "
                                   "changes; move the centre or make the region smaller")

    def bounding_box(self):
        """Equirectangular approximation around the centre; good to a few metres at these sizes."""
        half_lat = self.height_km / 2.0 / KM_PER_DEGREE_LAT
        km_per_degree_lon = KM_PER_DEGREE_LAT * math.cos(math.radians(self.center_lat))
        half_lon = self.width_km / 2.0 / km_per_degree_lon
        return BoundingBox(self.center_lon - half_lon, self.center_lat - half_lat,
                           self.center_lon + half_lon, self.center_lat + half_lat)

    def describe(self):
        return {"center": [self.center_lat, self.center_lon],
                "size_km": [self.width_km, self.height_km],
                "bbox": self.bounding_box().as_list()}


class RegionParser:
    """Turns the command line options into a Region."""

    @staticmethod
    def center(text):
        parts = text.split(",")
        if len(parts) != 2:
            raise argparse.ArgumentTypeError("expected LAT,LON")
        try:
            return float(parts[0]), float(parts[1])
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"not a number in {text!r}") from error

    @staticmethod
    def size(text):
        parts = text.lower().split("x")
        if len(parts) > 2:
            raise argparse.ArgumentTypeError("expected W or WxH in km")
        try:
            values = [float(part) for part in parts]
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"not a number in {text!r}") from error
        return values[0], values[-1]

    @staticmethod
    def region(args):
        width, height = args.size_km or (PRESETS_KM[args.preset],) * 2
        lat, lon = args.center
        return Region(lat, lon, width, height)


class FileHasher:
    """SHA-256 of files, read in chunks so large rasters do not fill the memory."""

    @staticmethod
    def sha256(path):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(HASH_CHUNK_BYTES), b""):
                digest.update(chunk)
        return digest.hexdigest()


class DatasetManifest:
    """perf-dataset.json: what was built, and the hash of each raster."""

    def __init__(self, directory):
        self.path = directory / MANIFEST_NAME
        self._directory = directory

    def write(self, region, image):
        content = {"format": MANIFEST_FORMAT, "region": region.describe(), "builder_image": image,
                   "files": {name: FileHasher.sha256(self._raster(name)) for name in RASTERS}}
        self.path.write_text(json.dumps(content, indent=2) + "\n", encoding="utf-8")
        return content

    def verify(self):
        """Returns the manifest when every raster matches it, raises DatasetError otherwise."""
        content = self._read()
        files = content.get("files")
        if not isinstance(files, dict) or sorted(files) != sorted(RASTERS):
            raise DatasetError(f"{self.path} does not list {', '.join(RASTERS)}")
        mismatched = [name for name in RASTERS if FileHasher.sha256(self._raster(name)) != files[name]]
        if mismatched:
            raise DatasetError(f"changed since the dataset was built: {', '.join(mismatched)}")
        return content

    def _read(self):
        try:
            content = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError as error:
            raise DatasetError(f"{self.path} is missing; run `build` first") from error
        except json.JSONDecodeError as error:
            raise DatasetError(f"{self.path} is not valid JSON: {error}") from error
        if not isinstance(content, dict) or content.get("format") != MANIFEST_FORMAT:
            raise DatasetError(f"{self.path} has an unknown format")
        if not isinstance(content.get("region"), dict):
            raise DatasetError(f"{self.path} has no region")
        return content

    def _raster(self, name):
        path = self._directory / name
        if not path.is_file():
            raise DatasetError(f"{path} is missing")
        return path


class TilesBuilder:
    """Runs the rfmap/tiles-builder image with the dataset directory mounted at /data."""

    def __init__(self, image, data_dir, runner=subprocess.run):
        if not IMAGE_PATTERN.match(image):
            raise DatasetError(f"not a valid image name: {image!r}")
        if ":" in str(data_dir):
            raise DatasetError(f"the dataset directory cannot contain ':' (Docker mount syntax): {data_dir}")
        self._image = image
        self._data_dir = data_dir
        self._runner = runner

    def build(self, box, force):
        """Writes dem.cog.tif and landcover.cog.tif into <data_dir>/out."""
        for step in self.commands(box, force):
            self._run(step)

    def commands(self, box, force):
        base = ["docker", "run", "--rm", *self._user_option(), "-v", f"{self._data_dir}:/data", self._image]
        extra = ["--force"] if force else []
        bbox = ["--bbox", box.as_argument()]
        return [[*base, "dem", *bbox, "--no-hillshade", "--no-contours", *extra],
                [*base, "landcover", *bbox, *extra]]

    @staticmethod
    def _user_option():
        # On Linux the outputs belong to the caller instead of root; Docker Desktop maps it itself.
        if hasattr(os, "getuid"):
            return ["--user", f"{os.getuid()}:{os.getgid()}"]
        return []

    def _run(self, command):
        if shutil.which(command[0]) is None:
            raise DatasetError("docker is not installed or not on PATH")
        try:
            result = self._runner(command, check=False, timeout=BUILD_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired as error:
            raise DatasetError(f"tiles-builder did not finish in {BUILD_TIMEOUT_SECONDS} s") from error
        if result.returncode != 0:
            raise DatasetError(f"tiles-builder failed (exit {result.returncode}): {' '.join(command[-6:])}")


class DatasetCli:
    """The command line: bbox, build and verify."""

    def __init__(self, builder_factory=TilesBuilder, output=sys.stdout):
        self._builder_factory = builder_factory
        self._output = output

    def parser(self):
        ap = argparse.ArgumentParser(prog="dataset.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
        sub = ap.add_subparsers(dest="command", required=True, metavar="COMMAND")
        bbox = sub.add_parser("bbox", help="print the bounding box of a region")
        self._region_options(bbox)
        build = sub.add_parser("build", help="build the rasters and write the manifest")
        self._region_options(build)
        build.add_argument("--out", type=Path, required=True, help="dataset directory, kept between runs")
        build.add_argument("--image", default=DEFAULT_IMAGE, help=f"tiles-builder image (default {DEFAULT_IMAGE})")
        build.add_argument("--force", action="store_true", help="rebuild even if tiles-builder sees no change")
        verify = sub.add_parser("verify", help="check the rasters against the manifest")
        verify.add_argument("--out", type=Path, required=True, help="dataset directory")
        return ap

    @staticmethod
    def _region_options(parser):
        size = parser.add_mutually_exclusive_group()
        size.add_argument("--preset", choices=sorted(PRESETS_KM), default=DEFAULT_PRESET,
                          help="square region: " + ", ".join(f"{k} = {v:g} km" for k, v in PRESETS_KM.items())
                               + f" (default {DEFAULT_PRESET})")
        size.add_argument("--size-km", type=RegionParser.size, help="W or WxH in km, instead of a preset")
        parser.add_argument("--center", type=RegionParser.center, default=DEFAULT_CENTER,
                            help="LAT,LON of the centre (default %(default)s, central Warsaw)")

    def main(self, argv=None):
        args = self.parser().parse_args(argv)
        handlers = {"bbox": self._bbox, "build": self._build, "verify": self._verify}
        try:
            handlers[args.command](args)
        except DatasetError as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        return 0

    def _bbox(self, args):
        region = RegionParser.region(args)
        region.validate()
        self._print(region.bounding_box().as_argument())

    def _build(self, args):
        region = RegionParser.region(args)
        region.validate()
        data_dir = args.out.resolve()
        data_dir.mkdir(parents=True, exist_ok=True)
        self._builder_factory(args.image, data_dir).build(region.bounding_box(), args.force)
        content = DatasetManifest(data_dir / "out").write(region, args.image)
        self._print(f"dataset ready in {data_dir / 'out'}")
        self._print(json.dumps(content["files"], indent=2))

    def _verify(self, args):
        content = DatasetManifest(args.out.resolve() / "out").verify()
        self._print(f"OK: {json.dumps(content['region'])}")

    def _print(self, text):
        print(text, file=self._output)


if __name__ == "__main__":
    sys.exit(DatasetCli().main())
