#!/usr/bin/env bash
# Smoke test: build every output for a tiny area with the real image.
#
#   tools/tiles-builder/tests/smoke.sh [DATA_DIR]
#
# Uses Planetiler's own Monaco test fixtures (OSM extract + small Natural Earth /
# water-polygon subsets) so the basemap part does not depend on Geofabrik, and a
# 0.1° x 0.06° DEM/landcover/satellite box near 48.53 N, 35.20 E.
# The satellite step uses ONE Sentinel-2 L2A scene (true colour) from the public
# sentinel-cogs bucket — a stand-in for a real cloudless mosaic, for testing only.
#
# Extra `docker run` arguments (e.g. proxy settings) can be passed in DOCKER_ARGS, separated by spaces.
set -euo pipefail

IMAGE="${IMAGE:-rfmap/tiles-builder}"
DATA="$(realpath -m "${1:-./smoke-data}")"
BBOX="35.15,48.50,35.25,48.56"
FIXTURES="https://raw.githubusercontent.com/onthegomap/planetiler/v0.10.2/planetiler-core/src/test/resources"
S2_SCENE="https://sentinel-cogs.s3.us-west-2.amazonaws.com/sentinel-s2-l2a-cogs/36/U/XU/2025/8/S2C_36UXU_20250813_0_L2A/TCI.tif"

mkdir -p "$DATA/inputs"
fetch() {  # fetch NAME URL: download URL into inputs/NAME unless it is already there
  local name=$1 url=$2
  [[ -s "$DATA/inputs/$name" ]] || curl -fsSL --proto =https --proto-redir =https -o "$DATA/inputs/$name" "$url"
}
for f in monaco-latest.osm.pbf natural_earth_vector.sqlite.zip water-polygons-split-3857.zip; do
  fetch "$f" "$FIXTURES/$f"
done
fetch s2-test-scene.tif "$S2_SCENE"

read -r -a docker_args <<< "${DOCKER_ARGS:-}"
run() { docker run --rm --user "$(id -u):$(id -g)" "${docker_args[@]}" -v "$DATA:/data" "$IMAGE" "$@"; }

run all --bbox "$BBOX" \
  --osm-file monaco-latest.osm.pbf --basemap-full-extract \
  --water-polygons-file water-polygons-split-3857.zip \
  --natural-earth-file natural_earth_vector.sqlite.zip \
  --java-heap 2g \
  --sat-file s2-test-scene.tif \
  --sat-name "Sentinel-2 L2A TCI, single scene S2C_36UXU_20250813 (TEST ONLY, not a cloudless mosaic)" \
  --sat-licence "Copernicus Sentinel data, free and open (test build)" \
  --sat-attribution "Contains modified Copernicus Sentinel data 2025"

# Second run must be a no-op (idempotency)
run all --bbox "$BBOX" \
  --osm-file monaco-latest.osm.pbf --basemap-full-extract \
  --water-polygons-file water-polygons-split-3857.zip \
  --natural-earth-file natural_earth_vector.sqlite.zip \
  --sat-file s2-test-scene.tif \
  --sat-name "Sentinel-2 L2A TCI, single scene S2C_36UXU_20250813 (TEST ONLY, not a cloudless mosaic)" \
  --sat-licence "Copernicus Sentinel data, free and open (test build)" \
  --sat-attribution "Contains modified Copernicus Sentinel data 2025" 2>&1 | grep -E "up to date|ERROR"

run verify
ls -la "$DATA/out"
echo "smoke test OK"
