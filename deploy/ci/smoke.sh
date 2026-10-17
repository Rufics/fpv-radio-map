#!/bin/sh
# Starts PostGIS, rfmap-server and web from freshly built images and checks that the stack
# answers through the web container, the same way a browser reaches it.
#   RFMAP_SERVER_IMAGE=... RFMAP_WEB_IMAGE=... deploy/ci/smoke.sh
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
compose() { docker compose -p rfmap-smoke --env-file "$work/.env" -f "$here/docker-compose.yml" -f "$here/ci/compose.smoke.yml" "$@"; }
trap 'compose logs --no-color rfmap-server web | tail -n 200; compose down -v >/dev/null 2>&1; rm -rf "$work"' EXIT
mkdir -p "$work/tiles"
admin=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
cat > "$work/.env" <<ENV
POSTGRES_PASSWORD=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
RFMAP_ADMIN_TOKEN=$admin
RFMAP_TILES_DIR=$work/tiles
RFMAP_HTTP_PORT=18088
ENV
compose up -d --wait --wait-timeout 180 postgis rfmap-server web

base=http://127.0.0.1:18088
check() {  # check NAME URL [curl args...]
  name=$1; url=$2; shift 2
  code=$(curl -s -o "$work/body" -w '%{http_code}' "$@" "$url")
  if [ "$code" != 200 ]; then echo "FAIL $name: HTTP $code"; cat "$work/body"; echo; exit 1; fi
  echo "ok   $name"
}
check "web app"            "$base/"
check "server health"      "$base/api/v1/health"
check "module list"        "$base/api/v1/modules"
check "radio profiles"     "$base/api/v1/library/radio-profiles"
check "admin cache"        "$base/api/v1/admin/cache" -H "Authorization: Bearer $admin"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/api/v1/admin/cache")
[ "$code" = 401 ] || { echo "FAIL admin without token: HTTP $code (want 401)"; exit 1; }
echo "ok   admin without token is refused"
