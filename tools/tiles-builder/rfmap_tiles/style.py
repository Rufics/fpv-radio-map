"""style: write out/style-dark.json (MapLibre, pmtiles:// sources) and copy glyph fonts."""
from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

from .common import BuildError, Dirs, log, read_meta, warn, write_meta

OUT_NAME = "style-dark.json"
TEMPLATE = Path(__file__).resolve().parent.parent / "styles" / "style-dark.template.json"
SOURCE_FILES = {"openmaptiles": "basemap.pmtiles", "satellite": "satellite.pmtiles",
                "hillshade": "hillshade.pmtiles", "contours": "contours.pmtiles"}


def add_arguments(p) -> None:
    g = p.add_argument_group("style")
    g.add_argument("--public-base-url", default="/tiles",
                   help="URL under which the web server serves out/ (default '/tiles'). Use an absolute "
                        "URL (https://host/tiles) if your MapLibre version rejects relative URLs.")
    g.add_argument("--style-keep-missing", action="store_true",
                   help="keep sources/layers whose .pmtiles was not built")


def build(args, dirs: Dirs) -> None:
    base = args.public_base_url.rstrip("/")
    if not TEMPLATE.is_file():
        raise BuildError(f"style template missing: {TEMPLATE}")
    style = json.loads(TEMPLATE.read_text(encoding="utf-8").replace("{{BASE_URL}}", base))

    attributions = []
    for src_id, fname in SOURCE_FILES.items():
        exists = (dirs.out / fname).exists()
        if not exists and not args.style_keep_missing:
            style["sources"].pop(src_id, None)
            style["layers"] = [l for l in style["layers"] if l.get("source") != src_id]
            log(f"style: {fname} not built; dropped source '{src_id}' and its layers")
            continue
        meta = read_meta(dirs, fname) or {}
        attr = meta.get("attribution_html") or meta.get("attribution")
        if attr:
            style["sources"][src_id]["attribution"] = attr
            if attr not in attributions:
                attributions.append(attr)
    wc = read_meta(dirs, "landcover.cog.tif")
    if wc and wc.get("attribution"):
        style["metadata"]["rfmap:landcover-attribution"] = wc["attribution"]

    tmp = dirs.out / f".tmp-{OUT_NAME}"
    tmp.write_text(json.dumps(style, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, dirs.out / OUT_NAME)
    log(f"wrote {dirs.out / OUT_NAME} (base URL {base})")
    write_meta(dirs, OUT_NAME, {"base_url": base}, {
        "kind": "maplibre-style", "source": "rfmap/tiles-builder styles/style-dark.template.json",
        "licence": "project file", "attribution": None,
        "sources": sorted(style["sources"]), "glyphs": style["glyphs"],
    })

    fonts_src = Path(os.environ.get("RFMAP_FONTS_DIR", "/opt/fonts"))
    fonts_dst = dirs.out / "fonts"
    if fonts_src.is_dir():
        shutil.copytree(fonts_src, fonts_dst, dirs_exist_ok=True)
        log(f"copied glyph fonts to {fonts_dst} (Noto Sans, SIL OFL 1.1 — see fonts/OFL.txt)")
    else:
        warn(f"glyph fonts not found at {fonts_src}; labels will not render until out/fonts exists")
