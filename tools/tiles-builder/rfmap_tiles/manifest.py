"""manifest: write out/manifest.json describing every output file."""
from __future__ import annotations

import json
import os

from .common import Dirs, log, now_iso, read_meta, sha256_file, tool_versions

OUT_NAME = "manifest.json"
FILES = ["basemap.pmtiles", "hillshade.pmtiles", "terrain.pmtiles", "contours.pmtiles", "satellite.pmtiles",
         "dem.cog.tif", "landcover.cog.tif", "style-dark.json"]
SKIP_META_KEYS = {"file", "params"}


def build(dirs: Dirs) -> dict:
    files = []
    for name in FILES:
        path = dirs.out / name
        if not path.exists():
            continue
        meta = read_meta(dirs, name) or {}
        entry = {
            "file": name,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        entry.update({k: v for k, v in meta.items() if k not in SKIP_META_KEYS and k not in ("bytes", "sha256")})
        if meta.get("sha256") and meta["sha256"] != entry["sha256"]:
            entry["warning"] = "file changed after it was built"
        files.append(entry)

    attributions = []
    for f in files:
        a = f.get("attribution")
        if a and a not in attributions:
            attributions.append(a)
    manifest = {
        "generated_at": now_iso(),
        "generator": "rfmap/tiles-builder",
        "tool_versions": tool_versions(),
        "calculation_inputs": {"dem": "dem.cog.tif", "landcover": "landcover.cog.tif",
                               "note": "the server reads these COGs directly; *.pmtiles are display only"},
        "attribution_display_text": " | ".join(attributions),
        "fonts": {"dir": "fonts/", "licence": "SIL Open Font License 1.1 (Noto Sans)",
                  "source": f"protomaps/basemaps-assets@{os.environ.get('RFMAP_FONTS_COMMIT', '?')}"}
                 if (dirs.out / "fonts").is_dir() else None,
        "files": files,
    }
    tmp = dirs.out / f".tmp-{OUT_NAME}"
    tmp.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, dirs.out / OUT_NAME)
    log(f"wrote {dirs.out / OUT_NAME} ({len(files)} files)")
    return manifest
