"""basemap: OpenStreetMap extract -> vector PMTiles (OpenMapTiles schema) with Planetiler."""
from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path

from . import sources
from .common import (BBox, BuildError, Dirs, download, existing_file, fetch_text, fresh_dir, heap_size, https_url,
                     install, is_up_to_date, log, pmtiles_header, run, safe_arg, safe_file, safe_int, safe_url,
                     write_meta, warn)

OUT_NAME = "basemap.pmtiles"
PLANETILER_JAR = "/opt/planetiler/planetiler.jar"


def add_arguments(p) -> None:
    g = p.add_argument_group("basemap (OpenStreetMap)")
    g.add_argument("--region", help="Geofabrik region: an id such as 'monaco' / 'ukraine' (looked up in the "
                                    "Geofabrik index), or a path such as 'europe/ukraine' "
                                    "(download.geofabrik.de/<path>-latest.osm.pbf)")
    g.add_argument("--osm-url", type=https_url, help="https URL of an .osm.pbf extract (overrides --region)")
    g.add_argument("--osm-file", type=existing_file,
                   help="local .osm.pbf file inside the data directory (overrides --region/--osm-url)")
    g.add_argument("--basemap-maxzoom", type=int, default=16,
                   help="highest zoom stored in basemap.pmtiles (default 16, Planetiler allows up to 16; "
                        "14 is ~4-10x smaller and MapLibre over-zooms it fine)")
    g.add_argument("--basemap-full-extract", action="store_true",
                   help="do not clip the basemap to --bbox; render the whole extract")
    g.add_argument("--water-polygons-file", type=existing_file,
                   help=f"local water-polygons-split-3857.zip (default: downloaded from {sources.WATER_POLYGONS_URL})")
    g.add_argument("--natural-earth-file", type=existing_file,
                   help=f"local natural_earth_vector.sqlite.zip (default: downloaded from {sources.NATURAL_EARTH_URL})")
    g.add_argument("--lake-centerlines-file", type=existing_file,
                   help=f"local lake_centerline.shp.zip (default: downloaded from {sources.LAKE_CENTERLINES_URL})")
    g.add_argument("--java-heap", type=heap_size, default=None,
                   help="Java max heap for Planetiler, e.g. 4g (default: 75%% of container RAM)")
    g.add_argument("--refresh-osm", action="store_true", help="re-download the OSM extract even if cached")


def md5_file(path: Path) -> str:
    """MD5 of a file, read in chunks. Geofabrik publishes only MD5 sums, so this checks the
    download for corruption; it is not a security check (the download itself is https)."""
    h = hashlib.md5(usedforsecurity=False)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _geofabrik_url(region: str) -> str:
    """The extract URL for a Geofabrik region path ('europe/monaco'), or the one the Geofabrik
    index lists for a region id ('monaco')."""
    if not re.match(r"^[a-z0-9_/-]+$", region) or region.startswith("-"):
        raise BuildError(f"--region looks wrong: {region!r} (use e.g. 'monaco' or 'europe/ukraine')")
    if "/" in region:
        return f"{sources.GEOFABRIK_BASE}/{region}-latest.osm.pbf"
    log(f"looking up Geofabrik region id '{region}'")
    try:
        index = json.loads(fetch_text(sources.GEOFABRIK_INDEX))
    except Exception as e:
        raise BuildError(f"cannot read the Geofabrik index ({e}); pass --region as a path like "
                         "'europe/monaco' or use --osm-url/--osm-file")
    for feature in index["features"]:
        props = feature["properties"]
        if props.get("id") == region:
            url = props.get("urls", {}).get("pbf", "")
            if not url.startswith(sources.GEOFABRIK_BASE + "/"):
                raise BuildError(f"the Geofabrik index lists an unexpected URL for '{region}': {url}")
            return url
    raise BuildError(f"no Geofabrik region with id '{region}'")


def _resolve_osm(args, dirs: Dirs) -> tuple[Path, str]:
    """Return (local pbf path, source description)."""
    if args.osm_file:
        pbf = safe_file(args.osm_file, "--osm-file")
        return pbf, f"file:{pbf.name}"
    if args.osm_url:
        url = safe_url(args.osm_url, "--osm-url")
    elif args.region:
        url = _geofabrik_url(args.region.strip("/"))
    else:
        raise BuildError("basemap needs --region, --osm-url or --osm-file")
    name = re.sub(r"[^A-Za-z0-9._-]", "_", url.split("?")[0].split("/")[-1]) or "extract.osm.pbf"
    key = hashlib.sha256(url.encode()).hexdigest()[:10]
    dest = dirs.cache / "osm" / f"{key}-{name}"
    download(url, dest, refresh=args.refresh_osm, untrusted=bool(args.osm_url))
    # Geofabrik publishes <file>.md5 next to each extract; check it when available.
    if url.startswith(sources.GEOFABRIK_BASE + "/"):
        try:
            expected = fetch_text(url + ".md5").split()[0]
            got = md5_file(dest)
            if expected != got:
                dest.unlink()
                raise BuildError(f"MD5 mismatch for {url} (expected {expected}, got {got}); deleted, retry")
            log("Geofabrik MD5 OK")
        except BuildError:
            raise
        except Exception as e:
            warn(f"could not check Geofabrik MD5: {e}")
    return dest, url


def _heap_flag(heap: str | None) -> str:
    """The JVM heap flag: -Xmx<size> for --java-heap, else a share of the container RAM."""
    if not heap:
        return "-XX:MaxRAMPercentage=75"
    text = safe_arg(heap, "--java-heap")
    if not re.match(r"^[1-9]\d{0,6}[kKmMgG]$", text, re.ASCII):
        raise BuildError(f"--java-heap must be a size such as 4g or 2048m (got {text!r})")
    return f"-Xmx{text}"


def build(args, dirs: Dirs) -> None:
    pbf, osm_source = _resolve_osm(args, dirs)
    bbox: BBox | None = None if args.basemap_full_extract or not args.bbox else BBox.parse(args.bbox)
    maxzoom = safe_int(args.basemap_maxzoom, "--basemap-maxzoom")
    if not 0 <= maxzoom <= 16:
        raise BuildError("--basemap-maxzoom must be 0..16")
    threads = safe_int(args.threads, "--threads") if args.threads else None

    params = {
        "osm_source": osm_source,
        "osm_size": pbf.stat().st_size,
        "osm_mtime": int(pbf.stat().st_mtime),
        "bounds": str(bbox) if bbox else "extract",
        "maxzoom": maxzoom,
    }
    if is_up_to_date(dirs, OUT_NAME, params, args.force):
        return

    aux = _aux_sources(args, dirs)
    work = fresh_dir(dirs.work / "planetiler")
    tmp_out = dirs.out / f".tmp-{OUT_NAME}"
    if tmp_out.exists():
        tmp_out.unlink()

    cmd = [
        "java", _heap_flag(args.java_heap),
        "-jar", PLANETILER_JAR,
        f"--osm-path={pbf}",
        f"--water-polygons-path={aux['water']}",
        f"--natural-earth-path={aux['natural_earth']}",
        f"--lake-centerlines-path={aux['lake_centerlines']}",
        f"--tmpdir={work}",
        f"--output={tmp_out}",
        f"--maxzoom={maxzoom}",
        f"--render-maxzoom={maxzoom}",
        "--force",
    ]
    if bbox:
        cmd.append(f"--bounds={bbox}")
    if threads:
        cmd.append(f"--threads={threads}")
    run(cmd)
    install(tmp_out, dirs, OUT_NAME)
    header = pmtiles_header(dirs.out / OUT_NAME)
    write_meta(dirs, OUT_NAME, params, {
        "kind": "vector-tiles",
        "schema": "OpenMapTiles (Planetiler built-in profile)",
        "source": f"{sources.OSM['source']}. Extract: {osm_source}",
        "licence": sources.OSM["licence"],
        "attribution": sources.OSM["attribution"],
        "attribution_html": sources.OSM["attribution_html"],
        "bbox": bbox.as_list() if bbox else _header_bbox(header),
        "zoom": [header.get("minzoom", 0), header.get("maxzoom", maxzoom)],
        "resolution": f"vector tiles z{header.get('minzoom', 0)}-{header.get('maxzoom', maxzoom)}",
    })
    shutil.rmtree(work, ignore_errors=True)


def _aux_sources(args, dirs: Dirs) -> dict[str, Path]:
    """Planetiler's auxiliary sources: the given files, else fetched with curl (retries, resume,
    proxy-friendly) into the cache."""
    src_dir = dirs.cache / "planetiler-sources"
    aux = {}
    for key, given, flag, url in (
            ("water", args.water_polygons_file, "--water-polygons-file", sources.WATER_POLYGONS_URL),
            ("natural_earth", args.natural_earth_file, "--natural-earth-file", sources.NATURAL_EARTH_URL),
            ("lake_centerlines", args.lake_centerlines_file, "--lake-centerlines-file", sources.LAKE_CENTERLINES_URL)):
        aux[key] = safe_file(given, flag) if given else _download_aux(url, src_dir, flag)
    return aux


def _download_aux(url: str, src_dir: Path, flag: str) -> Path:
    try:
        return download(url, src_dir / url.rsplit("/", 1)[1])
    except BuildError as e:
        raise BuildError(f"could not download {url}; download it elsewhere and pass {flag}") from e


def _header_bbox(h: dict) -> list | None:
    return h.get("bounds")
