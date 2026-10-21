"""The HTTP stand (stand/compose.yml): starts it, runs k6 in it, reads it and removes it.

Every scenario gets a stand of its own with an empty database, so runs start from the same
state. What the stand looked like goes into environment.json: the server's configuration file,
image and version, the containers' CPU and memory limits, and PostgreSQL's settings and sizes.

With monitoring (the default), Prometheus scrapes the server, the containers and PostgreSQL and
receives k6's metrics; with --keep-stand, Grafana shows them with the run's phases as
annotations. The stand's work folder, outside the run folder, holds the admin token Prometheus
sends and the dashboards Grafana loads; it goes with the stand.
"""
import json
import os
import secrets
import shutil
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from perfkit.api import ApiClient
from perfkit.commands import CommandRunner
from perfkit.errors import PerfError

TOOL_DIR = Path(__file__).resolve().parent.parent
COMPOSE_FILE = TOOL_DIR / "stand" / "compose.yml"
SANITIZE_FILE = TOOL_DIR / "stand" / "compose.sanitize.yml"
SANITIZED_IMAGE = "rfmap-server:perf-sanitize"
SERVER_CONFIG = TOOL_DIR / "stand" / "server.perf.json"
PROJECT = "rfmap-perf"
# The server seeds the library and migrates the database before it answers.
READY_TIMEOUT_S = 180
K6_THRESHOLDS_CROSSED = 99
MONITORING = ("prometheus", "cadvisor", "postgres-exporter")
# One scrape and one push of k6 after the load, so the last seconds are in Prometheus too.
SETTLE_S = 6
K6_REMOTE_WRITE = {"K6_PROMETHEUS_RW_SERVER_URL": "http://prometheus:9090/api/v1/write",
                   "K6_PROMETHEUS_RW_TREND_STATS": "p(50),p(95),p(99),max",
                   "K6_PROMETHEUS_RW_PUSH_INTERVAL": "5s"}
ANNOTATION_TAG = "rfmap-perf"
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
    monitoring: bool = True
    prometheus_port: int = 19090
    grafana_port: int = 13000
    sanitize: bool = False

    @property
    def grafana(self) -> bool:
        return self.monitoring and self.keep

    @property
    def image(self) -> str:
        """The server image; a sanitized build gets its own tag unless one was named."""
        if self.sanitize and self.server_image == "rfmap-server:perf":
            return SANITIZED_IMAGE
        return self.server_image


class StandLimits:
    """CPU and memory limits of the stand's containers, from RFMAP_PERF_* variables of the caller."""

    NAMES = ("RFMAP_PERF_SERVER_CPUS", "RFMAP_PERF_SERVER_MEMORY", "RFMAP_PERF_DB_CPUS", "RFMAP_PERF_DB_MEMORY",
             "RFMAP_PERF_K6_CPUS")

    @classmethod
    def from_environment(cls, environ: dict[str, str] | None = None) -> dict[str, str]:
        source = os.environ if environ is None else environ
        return {name: source[name] for name in cls.NAMES if name in source}


class Stand:
    def __init__(self, settings: StandSettings, runner: CommandRunner, admin_token: str = "", sleep=time.sleep):
        self.settings = settings
        self.admin_token = admin_token or secrets.token_urlsafe(32)
        self.run_dir: Path | None = None  # set by the suite once the run has its folder
        self._runner = runner
        self._sleep = sleep
        self._work_dir: Path | None = None

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.settings.port}/api/v1"

    @property
    def prometheus_url(self) -> str:
        return f"http://127.0.0.1:{self.settings.prometheus_port}/api/v1"

    @property
    def grafana_url(self) -> str:
        return f"http://127.0.0.1:{self.settings.grafana_port}"

    def extras(self) -> list[str]:
        """The optional services this stand runs, by the names collectors give in "needs"."""
        return [name for name, on in (("prometheus", self.settings.monitoring), ("sanitizer", self.settings.sanitize))
                if on]

    def endpoints(self) -> dict[str, str]:
        """What collectors can read besides the run's files: the stand's Prometheus, when it runs, and
        the folder of sanitizer reports of a sanitized server."""
        found = {"prometheus": self.prometheus_url} if self.settings.monitoring else {}
        if self.settings.sanitize:
            found["sanitizer"] = str(self.sanitizer_dir)
        return found

    @property
    def sanitizer_dir(self) -> Path:
        if self.run_dir is None:
            raise PerfError("the stand has no run folder yet")
        return self.run_dir / "raw" / "sanitizer"

    @property
    def work_dir(self) -> Path:
        if self._work_dir is None:
            self._work_dir = Path(tempfile.mkdtemp(prefix="rfmap-perf-"))
            # Readable by the containers that run as the caller; the token itself is 0600.
            self._work_dir.chmod(0o755)
            token = self._work_dir / "admin-token"
            token.touch(mode=0o600)
            token.write_text(self.admin_token, encoding="utf-8")
            (self._work_dir / "dashboards").mkdir()
        return self._work_dir

    def add_dashboard(self, name: str, source: Path) -> None:
        shutil.copyfile(source, self.work_dir / "dashboards" / f"{name}.json")

    def _profiles(self) -> list[str]:
        names = ["load"] + (["monitoring"] if self.settings.monitoring else []) + (
            ["grafana"] if self.settings.grafana else [])
        return [arg for name in names for arg in ("--profile", name)]

    def _services(self) -> list[str]:
        services = ["postgis", "rfmap-server"]
        if self.settings.monitoring:
            services += MONITORING
        if self.settings.grafana:
            services.append("grafana")
        return services

    def environment(self) -> dict[str, str]:
        s = self.settings
        user = f"{os.getuid()}:{os.getgid()}" if hasattr(os, "getuid") else "12345:12345"
        if self.run_dir is None:
            raise PerfError("the stand has no run folder yet")
        return {**os.environ, "RFMAP_PERF_DATA": str(s.dataset_dir), "RFMAP_PERF_RUN_DIR": str(self.run_dir),
                "RFMAP_PERF_PORT": str(s.port), "RFMAP_PERF_ADMIN_TOKEN": self.admin_token,
                "RFMAP_PERF_SERVER_IMAGE": s.image, "RFMAP_PERF_USER": user,
                "RFMAP_PERF_WORK_DIR": str(self.work_dir), "RFMAP_PERF_PROMETHEUS_PORT": str(s.prometheus_port),
                "RFMAP_PERF_GRAFANA_PORT": str(s.grafana_port),
                "RFMAP_PERF_SANITIZER_DIR": str(self.sanitizer_dir), **s.limits}

    def _compose(self, *args: str) -> list[str]:
        files = ["-f", str(COMPOSE_FILE)] + (["-f", str(SANITIZE_FILE)] if self.settings.sanitize else [])
        return ["docker", "compose", *files, "-p", PROJECT, *args]

    def up(self, client: ApiClient, clock=time.monotonic) -> None:
        build = "--build" if self.settings.build else "--no-build"
        if self.settings.sanitize:
            # Written by the server's user inside the container.
            self.sanitizer_dir.mkdir(parents=True, exist_ok=True)
            self.sanitizer_dir.chmod(0o777)
        self._runner.run(self._compose(*self._profiles(), "up", "-d", build, *self._services()),
                         env=self.environment())
        waits = [("rfmap-server", client, "/health")]
        if self.settings.monitoring:
            waits.append(("prometheus", ApiClient(self.prometheus_url), "/status/buildinfo"))
        if self.settings.grafana:
            waits.append(("grafana", ApiClient(self.grafana_url), "/api/health"))
        deadline = clock() + READY_TIMEOUT_S
        for service, service_client, path in waits:
            ReadyWait(service, service_client, path).until(deadline, clock, self._sleep)

    def load(self) -> bool:
        """Runs k6 with the plan in the run folder; False when the scenario's thresholds were crossed."""
        run_options: list[str] = []
        k6_options: list[str] = []
        if self.settings.monitoring:
            run_options = [arg for name, value in K6_REMOTE_WRITE.items() for arg in ("-e", f"{name}={value}")]
            k6_options = ["-o", "experimental-prometheus-rw"]
        code = self._runner.run(self._compose("--profile", "load", "run", "--rm", "--no-deps", *run_options, "k6",
                                              "run", "--quiet", *k6_options, "/scripts/runner.js"),
                                env=self.environment(), allowed=(0, K6_THRESHOLDS_CROSSED))
        return code == 0

    def stop_server(self) -> None:
        """Stops the server gracefully, so a sanitized one writes its leak report."""
        self._runner.run(self._compose("stop", "-t", "60", "rfmap-server"), env=self.environment())

    def wait(self, seconds: float) -> None:
        self._sleep(seconds)

    def server_pid(self) -> int:
        """The server's process id on this machine, for a profiler to attach to."""
        ids = self._runner.capture(self._compose("ps", "-q", "rfmap-server"), env=self.environment()).split()
        if not ids:
            raise PerfError("the stand's server is not running")
        pid = self._runner.capture(["docker", "inspect", "--format", "{{.State.Pid}}", ids[0]]).strip()
        if not pid.isdigit() or int(pid) == 0:
            raise PerfError(f"docker gave no process id for the server: {pid!r}")
        return int(pid)

    def settle(self) -> None:
        """Waits until Prometheus has the last seconds of the load."""
        if self.settings.monitoring:
            self._sleep(SETTLE_S)

    def annotate(self, events: list) -> None:
        """Marks the run's phases and stages in Grafana, where the dashboards show them."""
        if not self.settings.grafana:
            return
        client = ApiClient(self.grafana_url)
        for event in events:
            if not event.finished:
                continue
            client.expect(200, "POST", "/api/annotations", {
                "time": _epoch_ms(event.started), "timeEnd": _epoch_ms(event.finished),
                "tags": [ANNOTATION_TAG, event.kind], "text": f"{event.kind}: {event.name}"})

    def down(self) -> None:
        if self.settings.keep:
            return
        try:
            self._runner.run(self._compose(*self._profiles(), "down", "-v", "--remove-orphans"),
                             env=self.environment())
        finally:
            if self._work_dir is not None:
                shutil.rmtree(self._work_dir, ignore_errors=True)
                self._work_dir = None

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


class ReadyWait:
    """Polls one service of the stand until it answers 200."""

    def __init__(self, service: str, client: ApiClient, path: str):
        self._service = service
        self._client = client
        self._path = path

    def until(self, deadline: float, clock, sleep) -> None:
        while clock() < deadline:
            try:
                if self._client.call("GET", self._path).status == 200:
                    return
            except PerfError:
                pass  # not listening yet
            sleep(1)
        raise PerfError(f"{self._service} did not answer {self._path} within {READY_TIMEOUT_S} s; "
                        f"see docker compose -p {PROJECT} logs {self._service}")


def _epoch_ms(moment: str) -> int:
    return int(datetime.fromisoformat(moment).timestamp() * 1000)


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
