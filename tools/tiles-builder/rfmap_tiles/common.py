"""Shared helpers: paths, logging, subprocess, downloads, step metadata.

Every step writes its output to a temporary name and renames it into place
only when it is complete, and records its parameters in out/.meta/<file>.json.
A step whose output exists with identical parameters and checksum is skipped,
so re-running any command is safe (idempotent); --force rebuilds.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

USER_AGENT = "rfmap-tiles-builder/1.0 (FPV Radio Map, non-commercial; self-hosted map data build)"

# Web Mercator constants
EARTH_CIRCUMFERENCE = 2 * math.pi * 6378137.0
MERC_MAX = EARTH_CIRCUMFERENCE / 2


class BuildError(RuntimeError):
    """A failure the operator can act on; printed without a traceback."""


# --------------------------------------------------------------------------- logging

def log(msg: str) -> None:
    ts = _dt.datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", file=sys.stderr, flush=True)


def warn(msg: str) -> None:
    log(f"WARNING: {msg}")


def run(cmd: list[str], *, capture: bool = False, env: dict | None = None, cwd: str | Path | None = None) -> str:
    """Run a command, echoing it; raise BuildError on non-zero exit."""
    log("$ " + " ".join(shlex.quote(str(c)) for c in cmd))
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    res = subprocess.run([str(c) for c in cmd], env=full_env, cwd=cwd,
                         stdout=subprocess.PIPE if capture else None,
                         stderr=None, text=True)
    if res.returncode != 0:
        raise BuildError(f"command failed (exit {res.returncode}): {cmd[0]}")
    return res.stdout if capture else ""


# --------------------------------------------------------------------------- command-line values
# The build reads and writes only under DATA (the container's /data volume) and downloads only
# from the fixed URLs in sources.py. A value from the command line never becomes a path, a URL
# or a command argument itself: it only selects one of those trusted values.

DATA = Path("/data")
INPUTS_DIR = "inputs"

_HEAP_UNITS = {"k": "k", "m": "m", "g": "g"}


def input_file(text: str) -> Path:
    """argparse type: a file in /data/inputs, given by name or by its path in the container.
    Returns that directory's own entry, so the path used later is never the text typed."""
    inputs = DATA / INPUTS_DIR
    given = Path(text)
    if "/" in text and given.parent.resolve() != inputs.resolve():
        raise argparse_error(f"{text}: input files must be in {inputs}")
    if inputs.is_dir():
        for entry in inputs.iterdir():
            if entry.name == given.name and entry.is_file():
                return entry
    raise argparse_error(f"{text}: no such file in {inputs} (put input files in <data>/inputs/)")


def heap_size(text: str) -> str:
    """argparse type: a JVM heap size such as 4g or 2048m."""
    m = re.fullmatch(r"([1-9]\d{0,6})([kKmMgG])", text)
    if not m:
        raise argparse_error(f"must be a size such as 4g or 2048m: {text}")
    return f"{int(m.group(1))}{_HEAP_UNITS[m.group(2).lower()]}"


def argparse_error(message: str) -> argparse.ArgumentTypeError:
    return argparse.ArgumentTypeError(message)


# --------------------------------------------------------------------------- bbox

@dataclass(frozen=True)
class BBox:
    min_lon: float
    min_lat: float
    max_lon: float
    max_lat: float

    @classmethod
    def parse(cls, text: str) -> "BBox":
        try:
            parts = [float(p) for p in text.split(",")]
        except ValueError:
            raise BuildError(f"--bbox must be minLon,minLat,maxLon,maxLat (got {text!r})")
        if len(parts) != 4:
            raise BuildError(f"--bbox must have 4 numbers (got {text!r})")
        b = cls(*parts)
        if not (-180 <= b.min_lon < b.max_lon <= 180):
            raise BuildError("--bbox longitudes must satisfy -180 <= minLon < maxLon <= 180 "
                             "(areas crossing the antimeridian are not supported)")
        if not (-85 <= b.min_lat < b.max_lat <= 85):
            raise BuildError("--bbox latitudes must satisfy -85 <= minLat < maxLat <= 85")
        return b

    def as_list(self) -> list[float]:
        return [self.min_lon, self.min_lat, self.max_lon, self.max_lat]

    def __str__(self) -> str:
        return ",".join(f"{v:g}" for v in self.as_list())

    @property
    def center_lat(self) -> float:
        return (self.min_lat + self.max_lat) / 2

    def area_km2(self) -> float:
        h = (self.max_lat - self.min_lat) * 111.32
        w = (self.max_lon - self.min_lon) * 111.32 * math.cos(math.radians(self.center_lat))
        return w * h

    def mercator(self) -> tuple[float, float, float, float]:
        def x(lon): return math.radians(lon) * 6378137.0
        def y(lat): return math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)) * 6378137.0
        return x(self.min_lon), y(self.min_lat), x(self.max_lon), y(self.max_lat)


def zoom_resolution(z: int) -> float:
    """Metres per pixel (EPSG:3857 units) of a 256 px tile at zoom z."""
    return EARTH_CIRCUMFERENCE / 256 / (2 ** z)


def tile_aligned_extent(bbox: BBox, z: int) -> tuple[float, float, float, float]:
    """bbox in EPSG:3857, expanded outward to whole tiles of zoom z."""
    minx, miny, maxx, maxy = bbox.mercator()
    t = EARTH_CIRCUMFERENCE / (2 ** z)
    def fl(v): return math.floor((v + MERC_MAX) / t) * t - MERC_MAX
    def ce(v): return math.ceil((v + MERC_MAX) / t) * t - MERC_MAX
    return fl(minx), fl(miny), ce(maxx), ce(maxy)


# --------------------------------------------------------------------------- paths

@dataclass
class Dirs:
    data: Path
    out: Path
    cache: Path
    work: Path

    @property
    def meta(self) -> Path:
        return self.out / ".meta"

    @classmethod
    def make(cls) -> "Dirs":
        d = DATA
        dirs = cls(data=d, out=d / "out", cache=d / "cache", work=d / "work")
        for p in (dirs.out, dirs.cache, dirs.work, dirs.meta):
            p.mkdir(parents=True, exist_ok=True)
        return dirs


def fresh_dir(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


# --------------------------------------------------------------------------- downloads

def http_status(url: str, timeout: int = 60) -> int:
    """HEAD a URL; return the HTTP status (404 means 'tile does not exist')."""
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status
        except urllib.error.HTTPError as e:
            if e.code in (403, 404):  # S3 answers 403 for missing keys on some buckets
                return e.code
            if attempt == 3:
                raise BuildError(f"HEAD {url}: HTTP {e.code}")
        except (urllib.error.URLError, TimeoutError) as e:
            if attempt == 3:
                raise BuildError(f"HEAD {url}: {e}")
        time.sleep(2 ** attempt)
    return 0


def download(url: str, dest: Path, *, refresh: bool = False) -> Path:
    """Download url to dest with curl (resume + retries). Skips if dest exists."""
    if dest.exists() and dest.stat().st_size > 0 and not refresh:
        log(f"cached: {dest}")
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    if refresh and part.exists():
        part.unlink()
    # https only, redirects included.
    run(["curl", "-fL", "--proto", "=https", "--proto-redir", "=https", "--retry", "5", "--retry-delay", "5",
         "--retry-all-errors", "-C", "-", "-A", USER_AGENT, "-o", str(part), "--url", url])
    os.replace(part, dest)
    return dest


def fetch_text(url: str, timeout: int = 60) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8")


# --------------------------------------------------------------------------- metadata

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()


def meta_path(dirs: Dirs, out_name: str) -> Path:
    return dirs.meta / f"{out_name}.json"


def read_meta(dirs: Dirs, out_name: str) -> dict | None:
    p = meta_path(dirs, out_name)
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text())
    except json.JSONDecodeError:
        return None


def is_up_to_date(dirs: Dirs, out_name: str, params: dict, force: bool) -> bool:
    """True when out/<out_name> exists, was built with `params` and is unmodified."""
    if force:
        return False
    out = dirs.out / out_name
    meta = read_meta(dirs, out_name)
    if not out.exists() or not meta:
        return False
    if meta.get("params") != params:
        return False
    if meta.get("sha256") and meta["sha256"] != sha256_file(out):
        warn(f"{out_name} changed since it was built; rebuilding")
        return False
    log(f"{out_name} is up to date (same parameters); skipping. Use --force to rebuild.")
    return True


def write_meta(dirs: Dirs, out_name: str, params: dict, info: dict) -> None:
    out = dirs.out / out_name
    meta = {
        "file": out_name,
        "params": params,
        "built_at": now_iso(),
        "bytes": out.stat().st_size,
        "sha256": sha256_file(out),
        **info,
    }
    meta_path(dirs, out_name).write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")


def install(tmp: Path, dirs: Dirs, out_name: str) -> Path:
    """Atomically move a finished temp file into out/."""
    final = dirs.out / out_name
    os.replace(tmp, final)
    log(f"wrote {final} ({final.stat().st_size / 1e6:.1f} MB)")
    return final


# --------------------------------------------------------------------------- tool versions

def tool_versions() -> dict:
    def cmd(c):
        try:
            r = subprocess.run(c, capture_output=True, text=True, timeout=60)
            return (r.stdout + r.stderr).strip()
        except Exception:
            return None
    # `java -version` prints to stderr, possibly after a JAVA_TOOL_OPTIONS notice
    java = next((l for l in (cmd(["java", "-version"]) or "").splitlines() if " version " in l), None)
    pm = cmd(["pmtiles", "version"])
    return {
        "gdal": cmd(["gdalinfo", "--version"]),
        "planetiler": os.environ.get("RFMAP_PLANETILER_VERSION"),
        "pmtiles": pm.splitlines()[0] if pm else None,
        "java": java,
        "python": sys.version.split()[0],
        "fonts_commit": os.environ.get("RFMAP_FONTS_COMMIT"),
        "builder_image": os.environ.get("RFMAP_IMAGE_TAG", "rfmap/tiles-builder"),
    }


# --------------------------------------------------------------------------- tiles helpers

def mbtiles_set_metadata(mbtiles: Path, values: dict) -> None:
    import sqlite3
    con = sqlite3.connect(mbtiles)
    try:
        for k, v in values.items():
            con.execute("DELETE FROM metadata WHERE name = ?", (k,))
            con.execute("INSERT INTO metadata (name, value) VALUES (?, ?)", (k, str(v)))
        con.commit()
    finally:
        con.close()


def _normalize_mbtiles_zooms(mbtiles: Path) -> None:
    """Make minzoom/maxzoom/center metadata match the tiles that really exist.

    Small areas (or layers with no features at low zooms) produce no tiles at the
    requested minzoom; go-pmtiles copies the metadata into the header and
    `pmtiles verify` then rejects the archive.
    """
    import sqlite3
    con = sqlite3.connect(mbtiles)
    try:
        zmin, zmax = con.execute("SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles").fetchone()
        meta = dict(con.execute("SELECT name, value FROM metadata").fetchall())
    finally:
        con.close()
    if zmin is None:
        raise BuildError(f"{mbtiles.name}: no tiles were produced")
    values = {"minzoom": zmin, "maxzoom": zmax}
    try:
        b = [float(v) for v in meta.get("bounds", "").split(",")]
        values["center"] = f"{(b[0] + b[2]) / 2:.7f},{(b[1] + b[3]) / 2:.7f},{zmin}"
    except (ValueError, IndexError):
        pass
    mbtiles_set_metadata(mbtiles, values)


def mbtiles_to_pmtiles(mbtiles: Path, pmtiles: Path) -> None:
    _normalize_mbtiles_zooms(mbtiles)
    if pmtiles.exists():
        pmtiles.unlink()
    run(["pmtiles", "convert", str(mbtiles), str(pmtiles)])


def overview_factors(min_zoom: int, max_zoom: int) -> list[str]:
    return [str(2 ** i) for i in range(1, max_zoom - min_zoom + 1)]


def pmtiles_header(path: Path) -> dict:
    try:
        return json.loads(run(["pmtiles", "show", "--header-json", str(path)], capture=True))
    except (BuildError, json.JSONDecodeError):
        return {}
