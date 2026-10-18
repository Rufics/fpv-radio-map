"""Shared helpers: paths, logging, subprocess, downloads, step metadata.

Every step writes its output to a temporary name and renames it into place
only when it is complete, and records its parameters in out/.meta/<file>.json.
A step whose output exists with identical parameters and checksum is skipped,
so re-running any command is safe (idempotent); --force rebuilds.
"""
from __future__ import annotations

import argparse
import base64
import datetime as _dt
import hashlib
import http.client
import ipaddress
import json
import math
import os
import re
import shlex
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.parse
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
# Every value from the command line is validated before it reaches a path, a URL or a command:
#  * paths are canonicalised with os.path.realpath and must lie inside the directory the build was
#    started from (/data in the image), so an argument cannot point the build at other files;
#  * text and numbers that go on a tool's command line must match a strict character set and may
#    not start with "-", so they cannot add options to the command;
#  * URLs must be https, use URL characters only, and resolve to public addresses.

INPUTS_DIR = "inputs"
_ARG_TEXT = r"^[A-Za-z0-9_.,/:=+-]+$"
_URL_TEXT = r"^https://[A-Za-z0-9._~:/?#@!$&'()*+,;=%{}\[\]-]+$"


def safe_arg(value: object, what: str) -> str:
    """`value` as command-line text: letters, digits and _ . , / : = + - only, not starting with "-"."""
    text = str(value)
    if not re.match(_ARG_TEXT, text) or text.startswith("-"):
        raise BuildError(f"{what} {text!r}: use only letters, digits and _ . , / : = + - "
                         "(and do not start with -)")
    return text


def safe_int(value: object, what: str) -> int:
    """An integer from the command line, validated as safe_arg text first."""
    text = safe_arg(value, what)
    if not re.match(r"^\d+$", text, re.ASCII):
        raise BuildError(f"{what} must be a whole number >= 0 (got {text!r})")
    return int(text)


def safe_float(value: object, what: str) -> float:
    """A number from the command line, validated as safe_arg text first."""
    text = safe_arg(value, what)
    if not re.match(r"^\d+(\.\d+)?([eE][-+]?\d+)?$", text, re.ASCII):
        raise BuildError(f"{what} must be a number >= 0 (got {text!r})")
    return float(text)


def safe_path(path: str | os.PathLike, what: str) -> Path:
    """`path` resolved with os.path.realpath, refused unless it lies inside the working directory
    and its characters pass safe_arg (so the path can be handed to a tool as an argument)."""
    resolved = os.path.realpath(path)
    base_dir = os.path.realpath(os.getcwd())
    prefix = base_dir if base_dir.endswith(os.sep) else base_dir + os.sep  # "/" stays "/"
    if resolved != base_dir and not resolved.startswith(prefix):
        raise BuildError(f"{what} {str(path)!r} is outside the directory the build was started from "
                         f"({base_dir}); start the build from a directory that contains it")
    if not re.match(_ARG_TEXT, resolved):
        raise BuildError(f"{what} {resolved!r}: paths may use only letters, digits and _ . , / : = + -")
    return Path(resolved)


def safe_url(text: str, what: str) -> str:
    """An https URL from the command line: URL characters only, and its host must be public."""
    if not re.match(_URL_TEXT, text):
        raise BuildError(f"{what} must be an https:// URL without spaces or quotes (got {text!r})")
    public_host(text)
    return text


def _located(path: str | os.PathLike) -> str:
    """A relative file name that does not exist here is looked up in inputs/ as well."""
    text = os.fspath(path)
    if os.path.isabs(text) or os.path.exists(text):
        return text
    return os.path.join(INPUTS_DIR, text)


def safe_file(path: str | os.PathLike, what: str) -> Path:
    """safe_path for an existing regular file (a bare name may also be in inputs/)."""
    resolved = safe_path(_located(path), what)
    if not resolved.is_file():
        raise BuildError(f"{what}: not a file: {path}")
    return resolved


def existing_file(text: str) -> str:
    """argparse type: an existing regular file, here or in inputs/ (checked again with safe_file
    where it is used)."""
    if not Path(_located(text)).is_file():
        raise argparse_error(f"not a file: {text} (looked in the current directory and in {INPUTS_DIR}/)")
    return text


def directory(text: str) -> str:
    """argparse type: a directory to create or reuse (checked again with safe_path where it is used)."""
    if Path(text).expanduser().exists() and not Path(text).expanduser().is_dir():
        raise argparse_error(f"not a directory: {text}")
    return text


def https_url(text: str) -> str:
    """argparse type: an https URL with a host. Placeholders such as {z} may appear in the path."""
    parts = urllib.parse.urlsplit(text)
    if parts.scheme != "https" or not parts.hostname:
        raise argparse_error(f"must be an https:// URL: {text}")
    return text


def heap_size(text: str) -> str:
    """argparse type: a JVM heap size such as 4g or 2048m."""
    if not re.fullmatch(r"[1-9]\d{0,6}[kKmMgG]", text, re.ASCII):
        raise argparse_error(f"must be a size such as 4g or 2048m: {text}")
    return text


def argparse_error(message: str) -> argparse.ArgumentTypeError:
    return argparse.ArgumentTypeError(message)


def public_address(url: str) -> tuple[str, str]:
    """(host, address) for an https URL whose host resolves only to public addresses. The build
    fetches URLs from the command line, so it refuses loopback, private and link-local targets,
    and a host it cannot resolve itself (even when a proxy could)."""
    host = urllib.parse.urlsplit(url).hostname or ""
    try:
        infos = socket.getaddrinfo(host, 443, proto=socket.IPPROTO_TCP)
    except socket.gaierror as e:
        raise BuildError(f"cannot resolve {host}: {e}; download the file yourself and pass it as a local file")
    for info in infos:
        address = ipaddress.ip_address(info[4][0])
        if not address.is_global:
            raise BuildError(f"{host} resolves to {address}, which is not a public address")
    return host, infos[0][4][0]


def public_host(url: str) -> str:
    return public_address(url)[0]


class _PublicRedirect(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to an https URL on a public host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise BuildError(f"refusing a redirect to a non-https URL: {newurl}")
        public_host(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_OPENER = urllib.request.build_opener(_PublicRedirect)


def open_url(req: urllib.request.Request, timeout: int = 60):
    """urllib.request.urlopen, with every redirect hop checked by _PublicRedirect."""
    return _OPENER.open(req, timeout=timeout)


class _PinnedConnection(http.client.HTTPSConnection):
    """An HTTPS connection to one checked address. TLS and the Host header use the host name,
    and an http proxy is asked to CONNECT to the address, so the proxy does not resolve it."""

    def __init__(self, host: str, port: int, address: str, proxy: tuple[str, int, str] | None, timeout: int):
        super().__init__(host, port, timeout=timeout)
        self._address, self._proxy = address, proxy

    def connect(self) -> None:
        target = f"[{self._address}]:{self.port}" if ":" in self._address else f"{self._address}:{self.port}"
        if self._proxy is None:
            sock = socket.create_connection((self._address, self.port), self.timeout)
        else:
            proxy_host, proxy_port, auth = self._proxy
            sock = socket.create_connection((proxy_host, proxy_port), self.timeout)
            sock.sendall(f"CONNECT {target} HTTP/1.1\r\nHost: {target}\r\n{auth}\r\n".encode("ascii"))
            reply = b""
            while not reply.endswith(b"\r\n\r\n") and len(reply) < 16384:
                chunk = sock.recv(1)
                if not chunk:
                    break
                reply += chunk
            status = reply.split(b"\r\n", 1)[0]
            if status.split()[1:2] != [b"200"]:
                sock.close()
                raise OSError(f"proxy refused CONNECT {target}: {status.decode('latin-1')}")
        context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.check_hostname = True
        context.verify_mode = ssl.CERT_REQUIRED
        self.sock = context.wrap_socket(sock, server_hostname=self.host)


def _https_proxy(host: str) -> tuple[str, int, str] | None:
    """(host, port, Proxy-Authorization header line) of the http proxy for host, or None."""
    url = urllib.request.getproxies().get("https")
    if not url or urllib.request.proxy_bypass(host):
        return None
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "http" or not parts.hostname:
        raise BuildError("https_proxy must be a full proxy URL with the http scheme and a host name")
    auth = ""
    if parts.username:
        token = f"{urllib.parse.unquote(parts.username)}:{urllib.parse.unquote(parts.password or '')}"
        auth = f"Proxy-Authorization: Basic {base64.b64encode(token.encode()).decode('ascii')}\r\n"
    return parts.hostname, parts.port or 8080, auth


class PinnedPool:
    """Keep-alive HTTPS connections for many requests to one host. The host is resolved and
    checked once (public_address), and every connection goes to that address, so a later DNS
    answer cannot send a request elsewhere. Redirects are not followed."""

    def __init__(self, url: str, timeout: int = 60):
        self.host, self.address = public_address(url)
        self.port = urllib.parse.urlsplit(url).port or 443
        self.timeout = timeout
        self._idle: list[_PinnedConnection] = []

    def _path(self, url: str) -> str:
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "https" or parts.hostname != self.host or (parts.port or 443) != self.port:
            raise BuildError(f"{url}: not on {self.host}:{self.port}")
        return (parts.path or "/") + ("?" + parts.query if parts.query else "")

    def _connection(self) -> tuple[_PinnedConnection, bool]:
        """An idle connection (reused=True) or a new one to the checked address."""
        if self._idle:
            return self._idle.pop(), True
        return _PinnedConnection(self.host, self.port, self.address, _https_proxy(self.host), self.timeout), False

    def get(self, url: str) -> tuple[int, bytes]:
        """(status, body) of a GET; raises OSError or http.client.HTTPException on a transport error."""
        path = self._path(url)
        while True:
            conn, reused = self._connection()
            try:
                conn.request("GET", path, headers={"User-Agent": USER_AGENT})
                r = conn.getresponse()
                body = r.read()
            except (ConnectionResetError, BrokenPipeError):
                conn.close()
                if not reused:
                    raise
                # The server closed an idle keep-alive connection: retry on a new one.
                continue
            except BaseException:
                conn.close()
                raise
            if r.will_close:
                conn.close()
            else:
                self._idle.append(conn)
            return r.status, body

    def close(self) -> None:
        while self._idle:
            self._idle.pop().close()


# --------------------------------------------------------------------------- bbox

@dataclass(frozen=True)
class BBox:
    min_lon: float
    min_lat: float
    max_lon: float
    max_lat: float

    @classmethod
    def parse(cls, text: str) -> "BBox":
        if not re.match(r"^[-+0-9.,eE]+$", text):
            raise BuildError(f"--bbox must be minLon,minLat,maxLon,maxLat (got {text!r})")
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
    def make(cls, data: str, out: str | None = None) -> "Dirs":
        d = safe_path(data, "--data")
        dirs = cls(data=d, out=safe_path(out, "--out") if out else d / "out", cache=d / "cache", work=d / "work")
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
            with open_url(req, timeout=timeout) as r:
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


def download(url: str, dest: Path, *, refresh: bool = False, untrusted: bool = False) -> Path:
    """Download url to dest with curl (resume + retries). Skips if dest exists.
    An untrusted URL (from the command line) is fetched from the public address checked here,
    pinned with --resolve, and redirects are not followed."""
    if dest.exists() and dest.stat().st_size > 0 and not refresh:
        log(f"cached: {dest}")
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    if refresh and part.exists():
        part.unlink()
    # https only, redirects included for the fixed sources.
    follow = ["-L", "--proto-redir", "=https"]
    if untrusted:
        host, address = public_address(url)
        port = urllib.parse.urlsplit(url).port or 443
        pinned = f"[{address}]" if ":" in address else address
        # --connect-to sends the connection (or a proxy's CONNECT) to the checked address while
        # TLS stays on the host name. -L with --max-redirs 0 makes curl fail on a redirect
        # instead of saving its body.
        follow = ["-L", "--max-redirs", "0", "--connect-to", f"{host}:{port}:{pinned}:{port}"]
    # A refused redirect is not retried: --retry-all-errors is for the fixed sources only.
    retry = ["--retry", "5", "--retry-delay", "5"] + ([] if untrusted else ["--retry-all-errors"])
    run(["curl", "-f", *follow, "--proto", "=https", *retry,
         "-C", "-", "-A", USER_AGENT, "-o", str(part), "--url", url])
    os.replace(part, dest)
    return dest


def fetch_text(url: str, timeout: int = 60) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with open_url(req, timeout=timeout) as r:
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
