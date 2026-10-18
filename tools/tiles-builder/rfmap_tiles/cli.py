"""rfmap-tiles: single CLI entry point of the rfmap/tiles-builder image.

  rfmap-tiles basemap   --region europe/monaco [--bbox ...]
  rfmap-tiles dem       --bbox minLon,minLat,maxLon,maxLat
  rfmap-tiles landcover --bbox ...
  rfmap-tiles satellite --bbox ... --sat-provider NAME | --sat-file FILE --sat-mosaic NAME
                        | --sat-source TEMPLATE_OR_RASTER --sat-licence ... --sat-attribution ...
  rfmap-tiles all       --bbox ... [--region ...] [--sat-provider ... | --sat-file ... | --sat-source ...]
  rfmap-tiles style | manifest | verify

The build works inside the directory it is started from (/data in the image): every path given
on the command line must lie inside it. Downloads are cached in <data>/cache, outputs go to
<data>/out.
"""
from __future__ import annotations

import argparse
import os
import sys
import time

from . import basemap, dem, landcover, manifest, satellite, style
from .common import BBox, BuildError, Dirs, directory, log, run, warn


def _common(p: argparse.ArgumentParser) -> None:
    g = p.add_argument_group("common")
    g.add_argument("--bbox", help="minLon,minLat,maxLon,maxLat (WGS84 degrees)")
    g.add_argument("--data", type=directory, default=os.environ.get("RFMAP_DATA", "."),
                   help="data directory: cache/, work/ and out/ live here (default: $RFMAP_DATA, else the "
                        "current directory, which is /data in the image). Must lie inside the current directory.")
    g.add_argument("--out", type=directory, default=None,
                   help="output directory (default <data>/out). Must lie inside the current directory.")
    g.add_argument("--force", action="store_true", help="rebuild even if the output is up to date")
    g.add_argument("--threads", type=int, default=None, help="worker threads for Planetiler (default: all CPUs)")


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="rfmap-tiles", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True, metavar="COMMAND")
    specs = {
        "basemap": ("OSM extract -> basemap.pmtiles (Planetiler, OpenMapTiles schema)", [basemap, style]),
        "dem": ("Copernicus GLO-30 -> dem.cog.tif + hillshade.pmtiles + contours.pmtiles", [dem, style]),
        "landcover": ("ESA WorldCover 10 m -> landcover.cog.tif", [landcover, style]),
        "satellite": ("Sentinel-2 cloudless mosaic -> satellite.pmtiles", [satellite, style]),
        "all": ("run every step for one area", [basemap, dem, landcover, satellite, style]),
        "style": ("write style-dark.json and fonts/ for the files present", [style]),
        "manifest": ("rewrite manifest.json", [style]),
        "verify": ("validate the COGs and PMTiles in out/", [style]),
    }
    for name, (help_text, modules) in specs.items():
        p = sub.add_parser(name, help=help_text, description=help_text)
        _common(p)
        for m in modules:
            m.add_arguments(p)
    return ap


def verify(dirs: Dirs) -> bool:
    from . import raster
    ok = True
    for name in ("dem.cog.tif", "landcover.cog.tif"):
        p = dirs.out / name
        if not p.exists():
            continue
        errors = raster.validate_cog(p)
        if errors:
            ok = False
            log(f"FAIL {name}: not a valid COG: {errors}")
        else:
            s = raster.raster_summary(p)
            log(f"OK   {name}: valid COG, {s['size_px'][0]}x{s['size_px'][1]} {s['data_type']}, "
                f"block {s['block']}, {s['overviews']} overviews, {s['compression']}")
    for name in ("basemap.pmtiles", "hillshade.pmtiles", "contours.pmtiles", "satellite.pmtiles"):
        p = dirs.out / name
        if not p.exists():
            continue
        try:
            run(["pmtiles", "verify", str(p)])
            log(f"OK   {name}")
        except BuildError as e:
            ok = False
            log(f"FAIL {name}: {e}")
    return ok


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    t0 = time.time()
    try:
        if args.bbox:
            BBox.parse(args.bbox)  # fail early on a malformed bbox
        dirs = Dirs.make(args.data, args.out)
        cmd = args.command
        if cmd == "verify":
            return 0 if verify(dirs) else 1
        if cmd in ("basemap", "all"):
            if cmd == "all" and not (args.region or args.osm_file):
                warn("all: no --region/--osm-file given; skipping basemap")
            else:
                basemap.build(args, dirs)
        if cmd in ("dem", "all"):
            dem.build(args, dirs)
        if cmd in ("landcover", "all"):
            landcover.build(args, dirs)
        if cmd in ("satellite", "all"):
            if cmd == "all" and not (args.sat_provider or args.sat_file or args.sat_source):
                warn("all: no --sat-provider/--sat-file/--sat-source given; skipping satellite "
                     "(choose a mosaic and check its licence)")
            else:
                satellite.build(args, dirs)
        style.build(args, dirs)
        manifest.build(args, dirs)
        log(f"done in {time.time() - t0:.0f} s; outputs in {dirs.out}")
        return 0
    except BuildError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
