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
  code=$(curl -s -o "$work/body" -D "$work/headers" -w '%{http_code}' "$@" "$url")
  if [ "$code" != 200 ]; then echo "FAIL $name: HTTP $code"; cat "$work/body"; echo; exit 1; fi
  echo "ok   $name"
}
check "web app"            "$base/"
check "map worker"         "$base/maplibre/maplibre-gl-worker.mjs"
grep -qi '^content-type: text/javascript' "$work/headers" || { echo "FAIL map worker: not served as text/javascript"; cat "$work/headers"; exit 1; }
check "server health"      "$base/api/v1/health"
check "module list"        "$base/api/v1/modules"
check "radio profiles"     "$base/api/v1/library/radio-profiles"
check "admin cache"        "$base/api/v1/admin/cache" -H "Authorization: Bearer $admin"
code=$(curl -s -o /dev/null -w '%{http_code}' "$base/api/v1/admin/cache")
[ "$code" = 401 ] || { echo "FAIL admin without token: HTTP $code (want 401)"; exit 1; }
echo "ok   admin without token is refused"

# Rate limits count per client address; the "open" class (public link lookup) allows a burst of 5.
# Port 8080: a client cannot pick its address with X-Forwarded-For.
limited=no
for i in 1 2 3 4 5 6 7 8 9 10; do
  code=$(curl -s -o /dev/null -w '%{http_code}' -H "X-Forwarded-For: 198.51.100.$i" "$base/api/v1/links/smoke-unknown")
  if [ "$code" = 429 ]; then limited=yes; fi
done
[ "$limited" = yes ] || { echo "FAIL public port: a forged X-Forwarded-For escaped the rate limit"; exit 1; }
echo "ok   public port ignores a forged X-Forwarded-For"
# Port 8081 (the TLS proxy's, never published): the proxy's X-Forwarded-For names the client,
# so one client hitting the limit leaves the next one alone.
proxied() {  # proxied CLIENT_ADDRESS -> HTTP status of a link lookup through port 8081
  compose exec -T web wget -q -S -O /dev/null --header "X-Forwarded-For: $1" \
    http://127.0.0.1:8081/api/v1/links/smoke-unknown 2>&1 | awk '$1 ~ /^HTTP\// {code=$2} END {print code}'
}
for i in 1 2 3 4 5 6 7 8 9 10; do a=$(proxied 203.0.113.1); done
[ "$a" = 429 ] || { echo "FAIL proxy port: client A was not limited (HTTP $a)"; exit 1; }
b=$(proxied 203.0.113.2)
[ "$b" = 404 ] || { echo "FAIL proxy port: client B shares client A's limit (HTTP $b)"; exit 1; }
echo "ok   proxy port counts rate limits per client"
