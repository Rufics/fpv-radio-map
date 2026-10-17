#!/bin/sh
# Packs and signs every FPV module from a finished build, then runs the upload checks and the
# self-test of each package against the given owner public key.
#   pack-modules.sh BUILD_DIR OWNER.secret OWNER.pub OUT_DIR
set -eu
build=$1
key=$2
pub=$3
out=$4
rfmod="$build/platform/rfmod/rfmod"
mkdir -p "$out"

# Pack every staged module first; dependencies are needed by the checks below.
for stage in "$build"/modules/fpv/*/stage; do
  id=$(sed -n 's/^ *"id": *"\([^"]*\)".*/\1/p' "$stage/manifest.json" | head -1)
  version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$stage/manifest.json" | head -1)
  [ -n "$id" ] && [ -n "$version" ] || { echo "cannot read id/version in $stage/manifest.json" >&2; exit 1; }
  "$rfmod" pack "$stage" --key "$key" -o "$out/$id-$version.rfmod"
done

# Each package is checked with its required modules installed first, dependencies before
# dependents, as an admin would install them.
deps_in_order() {  # deps_in_order STAGE_ROOT ID -> stage dirs of the transitive requires
  python3 - "$1" "$2" <<'PY'
import json, pathlib, sys
root, target = pathlib.Path(sys.argv[1]), sys.argv[2]
manifests = {}
for m in root.glob("*/stage/manifest.json"):
    d = json.loads(m.read_text())
    manifests[d["id"]] = (d, m.parent)
order, seen = [], set()
def visit(i):
    for r in manifests[i][0].get("requires", []):
        if r["id"] not in seen:
            seen.add(r["id"])
            visit(r["id"])
            order.append(r["id"])
visit(target)
for i in order:
    d = manifests[i][0]
    print(f'{d["id"]}-{d["version"]}.rfmod')
PY
}
for stage in "$build"/modules/fpv/*/stage; do
  id=$(sed -n 's/^ *"id": *"\([^"]*\)".*/\1/p' "$stage/manifest.json" | head -1)
  version=$(sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p' "$stage/manifest.json" | head -1)
  pkg="$out/$id-$version.rfmod"
  set --
  for dep in $(deps_in_order "$build/modules/fpv" "$id"); do
    set -- "$@" --with "$out/$dep"
  done
  "$rfmod" test "$pkg" --trust "$pub" "$@" > "$pkg.check.txt" || true
  cat "$pkg.check.txt"
  grep -q "All checks passed" "$pkg.check.txt" || { echo "upload checks failed for $pkg" >&2; exit 1; }
  rm "$pkg.check.txt"
done
( cd "$out" && sha256sum *.rfmod > SHA256SUMS )
