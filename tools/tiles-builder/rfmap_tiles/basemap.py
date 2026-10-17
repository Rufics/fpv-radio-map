"""basemap: OpenStreetMap extract -> vector PMTiles (OpenMapTiles schema) with Planetiler."""
from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path

from . import sources
from .common import (BBox, BuildError, Dirs, download, fetch_text, fresh_dir, heap_size, input_file, install,
                     is_up_to_date, log, pmtiles_header, run, write_meta, warn)

OUT_NAME = "basemap.pmtiles"
PLANETILER_JAR = "/opt/planetiler/planetiler.jar"


def add_arguments(p) -> None:
    g = p.add_argument_group("basemap (OpenStreetMap)")
    g.add_argument("--region", help="Geofabrik region: an id such as 'monaco' / 'ukraine', or a path "
                                    "such as 'europe/ukraine', looked up in the Geofabrik index")
    g.add_argument("--osm-file", type=input_file,
                   help="an .osm.pbf extract in /data/inputs, for any other source (overrides --region)")
    g.add_argument("--basemap-maxzoom", type=int, default=16,
                   help="highest zoom stored in basemap.pmtiles (default 16, Planetiler allows up to 16; "
                        "14 is ~4-10x smaller and MapLibre over-zooms it fine)")
    g.add_argument("--basemap-full-extract", action="store_true",
                   help="do not clip the basemap to --bbox; render the whole extract")
    g.add_argument("--water-polygons-file", type=input_file,
                   help=f"water-polygons-split-3857.zip in /data/inputs (default: downloaded from {sources.WATER_POLYGONS_URL})")
    g.add_argument("--natural-earth-file", type=input_file,
                   help=f"natural_earth_vector.sqlite.zip in /data/inputs (default: downloaded from {sources.NATURAL_EARTH_URL})")
    g.add_argument("--lake-centerlines-file", type=input_file,
                   help=f"lake_centerline.shp.zip in /data/inputs (default: downloaded from {sources.LAKE_CENTERLINES_URL})")
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
    """The extract URL the Geofabrik index lists for a region id ('monaco') or path ('europe/monaco')."""
    log(f"looking up Geofabrik region '{region}'")
    try:
        index = json.loads(fetch_text(sources.GEOFABRIK_INDEX))
    except Exception as e:
        raise BuildError(f"cannot read the Geofabrik index ({e}); download the extract yourself and "
                         "pass it with --osm-file")
    by_path = f"{sources.GEOFABRIK_BASE}/{region}-latest.osm.pbf"
    for feature in index["features"]:
        props = feature["properties"]
        url = props.get("urls", {}).get("pbf", "")
        if props.get("id") == region or url == by_path:
            if not url.startswith(sources.GEOFABRIK_BASE + "/"):
                raise BuildError(f"the Geofabrik index lists an unexpected URL for '{region}': {url}")
            return url
    raise BuildError(f"no Geofabrik region '{region}' (use an id such as 'monaco' or a path such as "
                     "'europe/ukraine')")


def _resolve_osm(args, dirs: Dirs) -> tuple[Path, str]:
    """Return (local pbf path, source description)."""
    if args.osm_file:
        return args.osm_file, f"file:{args.osm_file.name}"
    if not args.region:
        raise BuildError("basemap needs --region or --osm-file")
    url = _geofabrik_url(args.region.strip("/"))
    name = re.sub(r"[^A-Za-z0-9._-]", "_", url.split("/")[-1]) or "extract.osm.pbf"
    key = hashlib.sha256(url.encode()).hexdigest()[:10]
    dest = dirs.cache / "osm" / f"{key}-{name}"
    download(url, dest, refresh=args.refresh_osm)
    # Geofabrik publishes <file>.md5 next to each extract; check it when available.
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


def build(args, dirs: Dirs) -> None:
    pbf, osm_source = _resolve_osm(args, dirs)
    bbox: BBox | None = None if args.basemap_full_extract or not args.bbox else BBox.parse(args.bbox)
    if not 0 <= args.basemap_maxzoom <= 16:
        raise BuildError("--basemap-maxzoom must be 0..16")

    params = {
        "osm_source": osm_source,
        "osm_size": pbf.stat().st_size,
        "osm_mtime": int(pbf.stat().st_mtime),
        "bounds": str(bbox) if bbox else "extract",
        "maxzoom": args.basemap_maxzoom,
    }
    if is_up_to_date(dirs, OUT_NAME, params, args.force):
        return

    # Planetiler's auxiliary sources, fetched with curl (retries, resume, proxy-friendly) into the cache.
    src_dir = dirs.cache / "planetiler-sources"
    aux = {}
    for key, given, flag, url in (
            ("water", args.water_polygons_file, "--water-polygons-file", sources.WATER_POLYGONS_URL),
            ("natural_earth", args.natural_earth_file, "--natural-earth-file", sources.NATURAL_EARTH_URL),
            ("lake_centerlines", args.lake_centerlines_file, "--lake-centerlines-file", sources.LAKE_CENTERLINES_URL)):
        if given:
            aux[key] = given
        else:
            try:
                aux[key] = download(url, src_dir / url.rsplit("/", 1)[1])
            except BuildError:
                raise BuildError(f"could not download {url}; download it elsewhere and pass {flag}")
    water, ne, lakes = aux["water"], aux["natural_earth"], aux["lake_centerlines"]

    work = fresh_dir(dirs.work / "planetiler")
    tmp_out = dirs.out / f".tmp-{OUT_NAME}"
    if tmp_out.exists():
        tmp_out.unlink()

    java = ["java"]
    java += [f"-Xmx{args.java_heap}"] if args.java_heap else ["-XX:MaxRAMPercentage=75"]
    cmd = java + [
        "-jar", PLANETILER_JAR,
        f"--osm-path={pbf}",
        f"--water-polygons-path={water}",
        f"--natural-earth-path={ne}",
        f"--lake-centerlines-path={lakes}",
        f"--tmpdir={work}",
        f"--output={tmp_out}",
        f"--maxzoom={args.basemap_maxzoom}",
        f"--render-maxzoom={args.basemap_maxzoom}",
        "--force",
    ]
    if bbox:
        cmd.append(f"--bounds={bbox}")
    if args.threads:
        cmd.append(f"--threads={args.threads}")
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
        "zoom": [header.get("minzoom", 0), header.get("maxzoom", args.basemap_maxzoom)],
        "resolution": f"vector tiles z{header.get('minzoom', 0)}-{header.get('maxzoom', args.basemap_maxzoom)}",
    })
    shutil.rmtree(work, ignore_errors=True)


def _header_bbox(h: dict) -> list | None:
    return h.get("bounds")
