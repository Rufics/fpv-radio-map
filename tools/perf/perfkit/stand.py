"""The HTTP stand (stand/compose.yml): starts it, runs k6 in it, reads it and removes it.

Every scenario gets a stand of its own with an empty database, so runs start from the same
state. What the stand looked like goes into environment.json: the server's configuration file,
image and version, the containers' CPU and memory limits, and PostgreSQL's settings and sizes.
"""
import json
import os
import secrets
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from perfkit.api import ApiClient
from perfkit.commands import CommandRunner
from perfkit.errors import PerfError

TOOL_DIR = Path(__file__).resolve().parent.parent
COMPOSE_FILE = TOOL_DIR / "stand" / "compose.yml"
SERVER_CONFIG = TOOL_DIR / "stand" / "server.perf.json"
PROJECT = "rfmap-perf"
# The server seeds the library and migrates the database before it answers.
READY_TIMEOUT_S = 180
K6_THRESHOLDS_CROSSED = 99
_PG_SETTINGS_SQL = ("select name, setting, coalesce(unit, '') from pg_settings where name in ('max_connections', "
                    "'shared_buffers', 'effective_cache_size', 'work_mem', 'maintenance_work_mem', 'random_page_cost', "
                    "'synchronous_commit', 'max_wal_size', 'checkpoint_timeout', 'jit')")
_TABLES_SQL = ("select relname, n_live_tup, pg_total_relation_size(relid) from pg_stat_user_tables "
               "order by 3 desc limit 20")


@dataclass(frozen=True)
class StandSettings:
    dataset_dir: Path
    port: int = 18080
    build: bool = True
    server_image: str = "rfmap-server:perf"
    keep: bool = False
    limits: dict[str, str] = field(default_factory=dict)  # RFMAP_PERF_*_CPUS / _MEMORY overrides


class StandLimits:
    """CPU and memory limits of the stand's containers, from RFMAP_PERF_* variables of the caller."""

    NAMES = ("RFMAP_PERF_SERVER_CPUS", "RFMAP_PERF_SERVER_MEMORY", "RFMAP_PERF_DB_CPUS", "RFMAP_PERF_DB_MEMORY",
             "RFMAP_PERF_K6_CPUS")

    @classmethod
    def from_environment(cls, environ: dict[str, str] | None = None) -> dict[str, str]:
        source = os.environ if environ is None else environ
        return {name: source[name] for name in cls.NAMES if name in source}


class Stand:
    def __init__(self, settings: StandSettings, runner: CommandRunner, admin_token: str = ""):
        self.settings = settings
        self.admin_token = admin_token or secrets.token_urlsafe(32)
        self.run_dir: Path | None = None  # set by the suite once the run has its folder
        self._runner = runner

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.settings.port}/api/v1"

    def environment(self) -> dict[str, str]:
        s = self.settings
        user = f"{os.getuid()}:{os.getgid()}" if hasattr(os, "getuid") else "12345:12345"
        if self.run_dir is None:
            raise PerfError("the stand has no run folder yet")
        return {**os.environ, "RFMAP_PERF_DATA": str(s.dataset_dir), "RFMAP_PERF_RUN_DIR": str(self.run_dir),
                "RFMAP_PERF_PORT": str(s.port), "RFMAP_PERF_ADMIN_TOKEN": self.admin_token,
                "RFMAP_PERF_SERVER_IMAGE": s.server_image, "RFMAP_PERF_USER": user, **s.limits}

    def _compose(self, *args: str) -> list[str]:
        return ["docker", "compose", "-f", str(COMPOSE_FILE), "-p", PROJECT, *args]

    def up(self, client: ApiClient, clock=time.monotonic, sleep=time.sleep) -> None:
        build = "--build" if self.settings.build else "--no-build"
        self._runner.run(self._compose("up", "-d", build, "postgis", "rfmap-server"), env=self.environment())
        deadline = clock() + READY_TIMEOUT_S
        while clock() < deadline:
            try:
                if client.call("GET", "/health").status == 200:
                    return
            except PerfError:
                pass  # not listening yet
            sleep(1)
        raise PerfError(f"rfmap-server did not answer /health within {READY_TIMEOUT_S} s; "
                        f"see docker compose -p {PROJECT} logs rfmap-server")

    def load(self) -> bool:
        """Runs k6 with the plan in the run folder; False when the scenario's thresholds were crossed."""
        code = self._runner.run(self._compose("--profile", "load", "run", "--rm", "--no-deps", "k6", "run",
                                              "--quiet", "/scripts/runner.js"),
                                env=self.environment(), allowed=(0, K6_THRESHOLDS_CROSSED))
        return code == 0

    def down(self) -> None:
        if self.settings.keep:
            return
        self._runner.run(self._compose("--profile", "load", "down", "-v", "--remove-orphans"), env=self.environment())

    def logs(self, path: Path) -> None:
        path.write_text(self._runner.capture(self._compose("logs", "--no-color", "rfmap-server"),
                                             env=self.environment()), encoding="utf-8")

    def psql(self, sql: str) -> list[list[str]]:
        out = self._runner.capture(self._compose("exec", "-T", "postgis", "psql", "-U", "rfmap", "-d", "rfmap",
                                                 "-AtF", "\t", "-c", sql), env=self.environment())
        return [line.split("\t") for line in out.splitlines() if line]

    def containers(self) -> dict[str, Any]:
        ids = self._runner.capture(self._compose("ps", "-q"), env=self.environment()).split()
        if not ids:
            return {}
        found = {}
        for entry in json.loads(self._runner.capture(["docker", "inspect", *ids])):
            host = entry.get("HostConfig", {})
            found[entry["Name"].lstrip("/")] = {"image": entry.get("Image", ""), "imageName": entry["Config"]["Image"],
                                                "nanoCpus": host.get("NanoCpus", 0),
                                                "memoryBytes": host.get("Memory", 0)}
        return found


class StandProbe:
    """What the stand looked like; read while it runs."""

    def __init__(self, stand: Stand, client: ApiClient):
        self._stand = stand
        self._client = client

    def server(self) -> dict[str, Any]:
        health = self._client.call("GET", "/health").body
        return {"config": ServerConfigFile.read(SERVER_CONFIG), "health": health,
                "containers": self._stand.containers()}

    def database(self) -> dict[str, Any]:
        # A unit like 8kB is the size of one step: shared_buffers 16384 (8kB) is 128 MB.
        settings = {row[0]: f"{row[1]} ({row[2]})" if len(row) > 2 and row[2] else row[1]
                    for row in self._stand.psql(_PG_SETTINGS_SQL)}
        return {"version": self._stand.psql("show server_version")[0][0], "settings": settings, **self.sizes()}

    def sizes(self) -> dict[str, Any]:
        tables = {row[0]: {"rows": int(row[1]), "bytes": int(row[2])} for row in self._stand.psql(_TABLES_SQL)}
        return {"databaseBytes": int(self._stand.psql("select pg_database_size(current_database())")[0][0]),
                "tables": tables}


class ServerConfigFile:
    """The stand's server configuration: JSON with // comment lines."""

    @staticmethod
    def read(path: Path) -> dict[str, Any]:
        lines = [line for line in path.read_text(encoding="utf-8").splitlines() if not line.strip().startswith("//")]
        return json.loads("\n".join(lines))
