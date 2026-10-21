"""One perf run from start to end, whatever the suite: the run folder, the timeline, the
environment snapshot, the history and the comparison with the baseline.

A suite (micro, and later the HTTP scenarios) says which items it uses and does the work; it
hands back what its collectors read. The session writes into <results>/<run id>/:

    environment.json   what the numbers depend on besides the code
    events.json        when each phase started and finished
    results.json       every measurement, its baseline and verdict
    raw/               what the tools wrote

and adds the run to <results>/history.sqlite.
"""
import json
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Protocol, TextIO

from perfkit.catalog import Item
from perfkit.compare import Baseline, Comparer, Comparison, Verdict
from perfkit.environment import Environment
from perfkit.errors import PerfError
from perfkit.history import History, RunRecord
from perfkit.measure import Collection, Measurement
from perfkit.timeline import Timeline, utc_now


@dataclass(frozen=True)
class Collected:
    """What one collector read for one item, and which of its metrics gate the run."""

    source: Item
    collector: Item
    collection: Collection
    gate_metrics: frozenset[str]
    limit_pct: float
    related: tuple[Item, ...] = ()

    @property
    def involved(self) -> dict[str, str]:
        items = (self.source, self.collector, *self.related)
        return {item.key: str(item.version) for item in items}

    def gates(self, measurement: Measurement) -> bool:
        """A gate names a metric of every measurement (p95) or of one (server_rss:slope)."""
        return (measurement.metric in self.gate_metrics
                or f"{measurement.name}:{measurement.metric}" in self.gate_metrics)


class Suite(Protocol):
    name: str
    dataset: str

    def items(self) -> list[Item]: ...

    def execute(self, timeline: Timeline, run_dir: Path) -> list[Collected]: ...

    def environment(self) -> dict[str, Any]: ...

    def problems(self) -> list[str]: ...


@dataclass(frozen=True)
class SessionSettings:
    results_dir: Path
    gate: bool = True
    window: int = 10


class RunSession:
    def __init__(self, settings: SessionSettings, environment: Environment, output: TextIO,
                 clock: Callable[[], str] = utc_now):
        self._settings = settings
        self._environment = environment
        self._output = output
        self._clock = clock

    def run(self, suite: Suite) -> int:
        items = {item.key: str(item.version) for item in suite.items()}
        timeline = Timeline(self._clock)
        run_dir = self._run_dir()
        try:
            with timeline.phase(f"{suite.name} run", "run"):
                collected = suite.execute(timeline, run_dir)
        finally:
            # Also after a failure, so it is clear which phase broke and when.
            self._write_json(run_dir / "events.json", timeline.to_json())
        environment = {**self._environment.snapshot(), **suite.environment()}
        for entry in collected:
            environment.setdefault("tools", {}).update(entry.collection.context)
        record = RunRecord(run_dir.name, suite.name, timeline.events()[0].started, timeline.events()[0].finished,
                           Environment.host_fingerprint(environment), suite.dataset,
                           environment.get("git", {}).get("commit", ""), items, environment,
                           [m for entry in collected for m in entry.collection.measurements], timeline.events())
        history = History.open(self._settings.results_dir)
        try:
            comparisons = self._compare(history, record, collected)
            history.record(record)
        finally:
            history.close()
        self._write(run_dir, record, timeline, comparisons)
        ConsoleTable(self._output).print(run_dir, comparisons)
        for problem in suite.problems():
            print(f"problem: {problem}", file=self._output)
        failed = [c for c in comparisons if c.gated and c.verdict is Verdict.REGRESSION]
        return 1 if (failed and self._settings.gate) or suite.problems() else 0

    def _run_dir(self) -> Path:
        stamp = self._clock().replace("-", "").replace(":", "").split(".")[0] + "Z"
        for attempt in range(1, 100):
            path = self._settings.results_dir / (stamp if attempt == 1 else f"{stamp}-{attempt}")
            if not path.exists():
                path.mkdir(parents=True)
                return path
        raise PerfError(f"too many runs started at {stamp} in {self._settings.results_dir}")

    def _compare(self, history: History, record: RunRecord, collected: list[Collected]) -> list[Comparison]:
        comparer = Comparer(Baseline(history, record.host, record.dataset, record.started), self._settings.window)
        return [comparer.compare(m, entry.involved, entry.limit_pct, entry.gates(m))
                for entry in collected for m in entry.collection.measurements]

    @staticmethod
    def _write(run_dir: Path, record: RunRecord, timeline: Timeline, comparisons: list[Comparison]) -> None:
        results = {"runId": record.run_id, "suite": record.suite, "host": record.host, "dataset": record.dataset,
                   "items": record.items, "results": [ResultRow.of(c) for c in comparisons]}
        files: dict[str, Any] = {"environment.json": record.environment, "events.json": timeline.to_json(),
                                 "results.json": results}
        for name, data in files.items():
            RunSession._write_json(run_dir / name, data)

    @staticmethod
    def _write_json(path: Path, data: Any) -> None:
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")


class ResultRow:
    @staticmethod
    def of(comparison: Comparison) -> dict[str, Any]:
        row = asdict(comparison.measurement)
        row.update(baseline=comparison.baseline, baselineRuns=comparison.runs, worsePct=comparison.worse_pct,
                   limitPct=comparison.limit_pct, gated=comparison.gated, verdict=comparison.verdict.value)
        return row


class ConsoleTable:
    """The gated measurements of a run as an aligned table, worst change first."""

    def __init__(self, output: TextIO):
        self._output = output

    def print(self, run_dir: Path, comparisons: list[Comparison]) -> None:
        gated = sorted((c for c in comparisons if c.gated), key=lambda c: -(c.worse_pct or 0.0))
        rows: list[tuple[str, ...]] = [("SOURCE", "NAME", "METRIC", "VALUE", "BASELINE", "WORSE BY", "VERDICT")]
        rows += [self._row(c) for c in gated]
        widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]))]
        for row in rows:
            print("  ".join(cell.ljust(width) for cell, width in zip(row, widths, strict=True)).rstrip(),
                  file=self._output)
        regressions = sum(c.verdict is Verdict.REGRESSION for c in gated)
        print(f"{len(gated)} gated measurement(s), {regressions} regression(s); results in {run_dir}",
              file=self._output)

    @staticmethod
    def _row(c: Comparison) -> tuple[str, ...]:
        m = c.measurement
        change = "-" if c.worse_pct is None else f"{c.worse_pct:+.1f}%"
        return m.source, m.name, m.metric, Quantity.text(m.value, m.unit), Quantity.text(c.baseline, m.unit), change, \
            c.verdict.value


class Quantity:
    """A value with its unit for people: nanoseconds become µs, ms or s when large."""

    _TIME_STEPS = ((1e9, "s"), (1e6, "ms"), (1e3, "µs"))

    @classmethod
    def text(cls, value: float | None, unit: str) -> str:
        if value is None:
            return "-"
        if unit == "ns":
            for scale, name in cls._TIME_STEPS:
                if abs(value) >= scale:
                    return f"{value / scale:.4g} {name}"
        return f"{value:.4g} {unit}"
