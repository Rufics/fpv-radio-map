#!/bin/sh
# Starts or stops a throw-away PostgreSQL 18 + PostGIS cluster for the server tests.
#   pg_fixture.sh start <dir> <port>    pg_fixture.sh stop <dir>
# PostgreSQL refuses to run as root, so under root the cluster runs as the postgres user.
set -eu
cmd=$1
dir=$2
bin=${PG_BIN:-/usr/lib/postgresql/18/bin}
as_pg() {
  if [ "$(id -u)" = 0 ]; then runuser -u postgres -- "$@"; else "$@"; fi
}
case "$cmd" in
  start)
    port=$3
    [ -f "$dir/data/postmaster.pid" ] && as_pg "$bin/pg_ctl" -D "$dir/data" stop -m immediate >/dev/null 2>&1 || true
    rm -rf "$dir"
    mkdir -p "$dir"
    [ "$(id -u)" = 0 ] && chown postgres "$dir"
    as_pg "$bin/initdb" -D "$dir/data" -A trust -U rfmap >/dev/null
    as_pg "$bin/pg_ctl" -D "$dir/data" -l "$dir/log" -w \
      -o "-p $port -k $dir -c listen_addresses=127.0.0.1 -c fsync=off" start >/dev/null
    "$bin/createdb" -h 127.0.0.1 -p "$port" -U rfmap rfmap_test
    echo "postgresql://rfmap@127.0.0.1:$port/rfmap_test"
    ;;
  stop)
    as_pg "$bin/pg_ctl" -D "$dir/data" stop -m fast >/dev/null 2>&1 || true
    rm -rf "$dir"
    ;;
  *)
    echo "usage: $0 start <dir> <port> | stop <dir>" >&2
    exit 2
    ;;
esac
