"""history.sqlite: every run and its measurements, so new numbers can be compared with old ones.

A run row keeps what decides whether two runs are comparable: the host fingerprint, the dataset
and the version of every catalog item the run used. The measurements and the timeline events
hang off the run. The file lives in the results folder and is never committed.
"""
import json
import sqlite3
from dataclasses import dataclass, field
from pathlib import Path

from perfkit.errors import PerfError
from perfkit.measure import Measurement
from perfkit.timeline import Event
from perfkit.version import Version

HISTORY_NAME = "history.sqlite"
# Raise both together: _SCHEMA ends by setting user_version to _SCHEMA_VERSION.
_SCHEMA_VERSION = 1
_SCHEMA = """
create table runs (
    id integer primary key,
    run_id text not null unique,
    suite text not null,
    started text not null,
    finished text not null,
    host text not null,
    dataset text not null,
    git_commit text not null,
    items text not null,
    environment text not null
);
create table measurements (
    run integer not null references runs(id) on delete cascade,
    source text not null,
    name text not null,
    metric text not null,
    unit text not null,
    better text not null,
    value real not null
);
create index measurements_key on measurements(source, name, metric);
create table events (
    run integer not null references runs(id) on delete cascade,
    name text not null,
    kind text not null,
    started text not null,
    finished text not null,
    outcome text not null
);
pragma user_version = 1;
"""


@dataclass
class RunRecord:
    run_id: str
    suite: str
    started: str
    finished: str
    host: str
    dataset: str
    git_commit: str
    items: dict[str, str]
    environment: dict = field(default_factory=dict)
    measurements: list[Measurement] = field(default_factory=list)
    events: list[Event] = field(default_factory=list)


@dataclass(frozen=True)
class PastValue:
    run_id: str
    started: str
    value: float
    versions: dict[str, str]


class History:
    """The SQLite file; open it with `History.open(results_dir)` and close it when done."""

    def __init__(self, connection: sqlite3.Connection):
        self._db = connection

    @classmethod
    def open(cls, folder: Path) -> "History":
        try:
            folder.mkdir(parents=True, exist_ok=True)
            connection = sqlite3.connect(folder / HISTORY_NAME)
        except (OSError, sqlite3.Error) as error:
            raise PerfError(f"cannot open {folder / HISTORY_NAME}: {error}") from error
        history = cls(connection)
        try:
            connection.execute("pragma foreign_keys = on")
            history._migrate()
        except sqlite3.Error as error:
            connection.close()
            raise PerfError(f"cannot use {folder / HISTORY_NAME}: {error}") from error
        except PerfError:
            connection.close()
            raise
        return history

    def close(self) -> None:
        self._db.close()

    def _migrate(self) -> None:
        found = self._db.execute("pragma user_version").fetchone()[0]
        if found == 0:
            self._db.executescript(_SCHEMA)
        elif found != _SCHEMA_VERSION:
            raise PerfError(f"{HISTORY_NAME} has schema {found}, this toolchain knows {_SCHEMA_VERSION}")

    def record(self, run: RunRecord) -> None:
        with self._db:
            cursor = self._db.execute(
                "insert into runs(run_id, suite, started, finished, host, dataset, git_commit, items, environment)"
                " values (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (run.run_id, run.suite, run.started, run.finished, run.host, run.dataset, run.git_commit,
                 json.dumps(run.items, sort_keys=True), json.dumps(run.environment, sort_keys=True)))
            row = cursor.lastrowid
            self._db.executemany(
                "insert into measurements(run, source, name, metric, unit, better, value) values (?, ?, ?, ?, ?, ?, ?)",
                [(row, m.source, m.name, m.metric, m.unit, m.better, m.value) for m in run.measurements])
            self._db.executemany(
                "insert into events(run, name, kind, started, finished, outcome) values (?, ?, ?, ?, ?, ?)",
                [(row, e.name, e.kind, e.started, e.finished, e.outcome) for e in run.events])

    def past(self, measurement: Measurement, host: str, dataset: str, before: str) -> list[PastValue]:
        """Earlier values of one measurement on the same host and dataset, newest first."""
        rows = self._db.execute(
            "select r.run_id, r.started, m.value, r.items from measurements m join runs r on r.id = m.run"
            " where m.source = ? and m.name = ? and m.metric = ? and r.host = ? and r.dataset = ? and r.started < ?"
            " order by r.started desc",
            (measurement.source, measurement.name, measurement.metric, host, dataset, before)).fetchall()
        return [PastValue(run_id, started, value, json.loads(items)) for run_id, started, value, items in rows]

    @staticmethod
    def same_majors(versions: dict[str, str], wanted: dict[str, str]) -> bool:
        """True when every wanted item was used in the past run with the same major version."""
        for key, version in wanted.items():
            if key not in versions or Version.parse(versions[key]).major != Version.parse(version).major:
                return False
        return True
