#!/usr/bin/env python3
"""Moves the database of a running FPV Radio Map stack from PostgreSQL 16 to PostgreSQL 18.

    python3 -I deploy/pg_upgrade.py [--backup-dir DIR] [--yes]

Run it once, after `git pull` has brought the PostgreSQL 18 compose file and before the first
`docker compose up`. A new major version cannot open the old data folder, so the data moves by
dump and restore into a new volume:

1. rfmap-server and postgis are stopped, so nothing writes while the dump runs.
2. A temporary PostgreSQL 16 container, without a network, opens the old volume and writes a
   pg_dump (custom format) to the backup folder.
3. postgis starts on PostgreSQL 18 with its new volume, the database is created again empty (the
   first start fills it with the extensions the dump brings itself), and the dump is restored in
   one transaction.
4. The rows of every application table are counted on both sides and must match.
5. The whole stack is started again.

The old volume is opened only by PostgreSQL 16 and is left in place, so going back is checking
out the previous compose file and starting the stack. Remove it once the new database has run
for a while (the tool prints the command). The exit code is 0 on success, 1 when a step fails and 2 on
a usage error or a refused start.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEPLOY_DIR = Path(__file__).resolve().parent
PROJECT = "rfmap"
OLD_VOLUME = f"{PROJECT}_pgdata"
NEW_VOLUME = f"{PROJECT}_pgdata18"
# The image the stack ran before the move (deploy/docker-compose.yml up to PostgreSQL 16).
OLD_IMAGE = "postgis/postgis:16-3.5@sha256:94146ac37bc61e2322f88016056c5920729cb8c64c8542ed590af8fc2abdac07"
OLD_CONTAINER = f"{PROJECT}-pg16-upgrade"
DATABASE = "rfmap"
DB_USER = "rfmap"
COMMAND_TIMEOUT_SECONDS = 3600
READY_TIMEOUT_SECONDS = 180
READY_POLL_SECONDS = 2

RECOVERY_HINT = (
    f"Volume {OLD_VOLUME} still holds the PostgreSQL 16 data. To go back to PostgreSQL 16, check out "
    "the previous deploy/docker-compose.yml and run `docker compose up -d`. To try again: "
    f"`docker compose stop postgis && docker compose rm -f postgis && docker volume rm {NEW_VOLUME}`, "
    "then run this tool again."
)

# Rows per application table. Tables that belong to an extension (spatial_ref_sys and the like)
# are left out: their contents come with the extension version, not from the application.
ROW_COUNTS_SQL = """
select c.relname,
       (xpath('/row/n/text()',
              query_to_xml(format('select count(*) as n from %I.%I', n.nspname, c.relname),
                           false, true, '')))[1]::text::bigint
from pg_class c
join pg_namespace n on n.oid = c.relnamespace
where n.nspname = 'public' and c.relkind in ('r', 'p')
  and not exists (select 1 from pg_depend d
                  where d.classid = 'pg_class'::regclass and d.objid = c.oid and d.deptype = 'e')
order by c.relname
"""


class UpgradeError(Exception):
    """A step of the move failed; the message says which and why."""


class Docker:
    """Runs the docker CLI with an argument list (never through a shell) and checks the result."""

    def __init__(self, executable=None):
        self._executable = executable

    def _resolved(self):
        # The absolute path, found once, so no later call depends on the search path.
        if self._executable is None:
            self._executable = shutil.which("docker")
            if self._executable is None:
                raise UpgradeError("docker is not installed or not on PATH")
        return self._executable

    def run(self, args, stdin=None, stdout=None):
        """Runs `docker ARGS`, returns its standard output, raises UpgradeError when it fails."""
        command = [self._resolved(), *args]
        try:
            done = subprocess.run(command, stdin=stdin, stdout=stdout or subprocess.PIPE,
                                  stderr=subprocess.PIPE, check=False, timeout=COMMAND_TIMEOUT_SECONDS)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise UpgradeError(f"`docker {' '.join(args)}` could not run: {error}") from error
        if done.returncode != 0:
            message = done.stderr.decode(errors="replace").strip()
            raise UpgradeError(f"`docker {' '.join(args)}` failed ({done.returncode}): {message}")
        return done.stdout.decode(errors="replace") if done.stdout is not None else ""

    def succeeds(self, args):
        """True when `docker ARGS` exits with 0."""
        try:
            self.run(args)
        except UpgradeError:
            return False
        return True

    def volume_exists(self, name):
        return self.succeeds(["volume", "inspect", name])


class Compose:
    """`docker compose` for the stack in deploy/, under its fixed project name."""

    def __init__(self, docker, directory):
        self._docker = docker
        self._prefix = ["compose", "--project-directory", str(directory), "-p", PROJECT]

    def run(self, *args, stdin=None):
        return self._docker.run([*self._prefix, *args], stdin=stdin)

    def stop(self, *services):
        self.run("stop", *services)

    def start_and_wait(self, service):
        # Waits for the healthcheck, which runs over TCP and so passes only once the image has
        # finished initialising a new database (its temporary init server has no TCP listener).
        self.run("up", "-d", "--wait", "--wait-timeout", str(READY_TIMEOUT_SECONDS), service)

    def start_all(self):
        # Without --wait, as the docs start the stack: open-meteo-init exits once its download is
        # done, and --wait can count that as a failure.
        self.run("up", "-d")

    def postgis(self, *command, stdin=None):
        return self.run("exec", "-T", "postgis", *command, stdin=stdin)


class RowCounts:
    """Rows per application table, as `psql -At` prints ROW_COUNTS_SQL."""

    def __init__(self, counts):
        self.counts = counts

    @staticmethod
    def query():
        """The psql command line that prints ROW_COUNTS_SQL as `table|rows` lines."""
        return ["psql", "-U", DB_USER, "-d", DATABASE, "-At", "-v", "ON_ERROR_STOP=1",
                "-c", ROW_COUNTS_SQL]

    @classmethod
    def parse(cls, text):
        counts = {}
        for line in text.splitlines():
            if line.strip():
                table, _, rows = line.partition("|")
                counts[table] = int(rows)
        return cls(counts)

    def differences(self, other):
        """Lines naming each table whose row count differs, or that only one side has."""
        tables = sorted(set(self.counts) | set(other.counts))
        return [f"{table}: {self._rows(table)} before, {other._rows(table)} after"
                for table in tables if self.counts.get(table) != other.counts.get(table)]

    def _rows(self, table):
        return self.counts.get(table, "missing")

    @property
    def total(self):
        return sum(self.counts.values())


class OldCluster:
    """The PostgreSQL 16 data, opened by a temporary container without a network to dump it."""

    def __init__(self, docker, volume, image):
        self._docker = docker
        self._volume = volume
        self._image = image

    def __enter__(self):
        self._docker.run(["run", "-d", "--rm", "--name", OLD_CONTAINER, "--network", "none",
                          "-v", f"{self._volume}:/var/lib/postgresql/data", self._image])
        ready = False
        try:
            self._wait_ready()
            ready = True
        finally:
            # __exit__ does not run when __enter__ fails; a container left running would hold the
            # old volume and block the next try with a name conflict.
            if not ready:
                self._docker.succeeds(["stop", OLD_CONTAINER])
        return self

    def __exit__(self, *exc):
        self._docker.succeeds(["stop", OLD_CONTAINER])
        return False

    def _wait_ready(self):
        deadline = time.monotonic() + READY_TIMEOUT_SECONDS
        ready = ["exec", OLD_CONTAINER, "pg_isready", "-U", DB_USER, "-d", DATABASE]
        while not self._docker.succeeds(ready):
            if time.monotonic() > deadline:
                raise UpgradeError(f"PostgreSQL 16 on {self._volume} did not get ready")
            time.sleep(READY_POLL_SECONDS)

    def dump(self, target):
        # The dump holds the whole database: readable by the owner only, whatever the umask.
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as out:
            self._docker.run(["exec", OLD_CONTAINER, "pg_dump", "-U", DB_USER, "-Fc", DATABASE],
                             stdout=out)

    def row_counts(self):
        return RowCounts.parse(self._docker.run(["exec", OLD_CONTAINER, *RowCounts.query()]))


class Upgrade:
    """The steps of the move, in order; each one says what it does before it starts."""

    def __init__(self, docker, compose, backup_dir, out=print):
        self._docker = docker
        self._compose = compose
        self._backup_dir = backup_dir
        self._say = out

    def preflight(self):
        """Refuses to start when there is nothing to move or the move already happened."""
        if not self._docker.volume_exists(OLD_VOLUME):
            raise UpgradeError(f"volume {OLD_VOLUME} does not exist: nothing to move "
                               "(a fresh install starts on PostgreSQL 18 by itself)")
        if self._docker.volume_exists(NEW_VOLUME):
            raise UpgradeError(f"volume {NEW_VOLUME} exists already: the move has run before. "
                               f"Remove it first only if you are sure it holds nothing you need")

    def run(self):
        self._backup_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        dump = self._backup_dir / f"{DATABASE}-pg16-{time.strftime('%Y%m%d-%H%M%S')}.dump"
        self._say("Stopping rfmap-server and postgis")
        self._compose.stop("rfmap-server", "postgis")
        with OldCluster(self._docker, OLD_VOLUME, OLD_IMAGE) as old:
            self._say(f"Dumping the PostgreSQL 16 database to {dump}")
            old.dump(dump)
            before = old.row_counts()
        self._say(f"Restoring into PostgreSQL 18 ({len(before.counts)} tables, {before.total} rows)")
        self._compose.start_and_wait("postgis")
        self._recreate_database()
        with dump.open("rb") as source:
            self._compose.postgis("pg_restore", "-U", DB_USER, "-d", DATABASE, "--no-owner",
                                  "--single-transaction", "--exit-on-error", stdin=source)
        self._check(before)
        self._say("Starting the stack")
        self._compose.start_all()
        self._say(f"Done. The old data stays in volume {OLD_VOLUME} and in {dump}.\n"
                  f"Once the new database has run for a while: docker volume rm {OLD_VOLUME}")

    def _recreate_database(self):
        # The image's first start fills the new database with its own extensions and their schemas
        # (tiger, topology), which the dump creates again. Restore into an empty one instead.
        self._compose.postgis("psql", "-U", DB_USER, "-d", "postgres", "-v", "ON_ERROR_STOP=1",
                              "-c", f"drop database {DATABASE} with (force)",
                              "-c", f"create database {DATABASE} template template0 owner {DB_USER}")

    def _check(self, before):
        after = RowCounts.parse(self._compose.postgis(*RowCounts.query()))
        differences = before.differences(after)
        if differences:
            raise UpgradeError("row counts differ after the restore:\n  " + "\n  ".join(differences))
        self._say("Row counts match")


class Command:
    """Command line: parses the arguments, asks for confirmation, runs the move."""

    @staticmethod
    def _parser():
        parser = argparse.ArgumentParser(description="Move the rfmap database from PostgreSQL 16 to 18.")
        parser.add_argument("--backup-dir", type=Path, default=Path.home() / "rfmap-backup",
                            help="folder for the dump (default: ~/rfmap-backup)")
        parser.add_argument("--yes", action="store_true", help="do not ask before stopping the stack")
        return parser

    @staticmethod
    def _confirmed(ask):
        return ask("The stack will be down during the move. Continue? [y/N] ").strip().lower() == "y"

    def run(self, argv, ask=input):
        args = self._parser().parse_args(argv)
        docker = Docker()
        upgrade = Upgrade(docker, Compose(docker, DEPLOY_DIR), args.backup_dir.expanduser().resolve())
        try:
            upgrade.preflight()
        except UpgradeError as error:
            print(f"pg_upgrade: {error}", file=sys.stderr)
            return 2
        if not args.yes and not self._confirmed(ask):
            print("pg_upgrade: nothing changed", file=sys.stderr)
            return 2
        try:
            upgrade.run()
        except (UpgradeError, OSError) as error:
            print(f"pg_upgrade: {error}", file=sys.stderr)
            print(RECOVERY_HINT, file=sys.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(Command().run(sys.argv[1:]))
