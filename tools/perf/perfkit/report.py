"""Reports: what reporter extensions read about a finished run, and how they are run.

A reporter is an extension (extensions/reporters/<id>/) whose entry class derives from
Reporter. It gets a ReportContext: the files of one run folder (results, events, environment and
raw data of the collectors) and the trend of every measurement from history.sqlite, and writes
its files into the run folder. Reports are made at the end of `perf.py run` and again with
`perf.py report RUN_DIR`, so a new reporter can render old runs.
"""
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

from perfkit.catalog import Item
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.history import HISTORY_NAME, History, PastValue
from perfkit.measure import Measurement

DEFAULT_TREND_RUNS = 30


@dataclass(frozen=True)
class RunFiles:
    """The JSON files RunSession wrote into one run folder."""

    run_dir: Path
    results: dict[str, Any]
    events: list[dict[str, Any]]
    environment: dict[str, Any]

    @classmethod
    def read(cls, run_dir: Path) -> "RunFiles":
        return cls(run_dir, cls._json(run_dir / "results.json"), cls._json(run_dir / "events.json"),
                   cls._json(run_dir / "environment.json"))

    def raw(self, name: str) -> Any | None:
        """A JSON file a collector saved in raw/, or None when this run has none."""
        path = self.run_dir / "raw" / name
        return self._json(path) if path.is_file() else None

    @property
    def run(self) -> dict[str, Any]:
        """The event of the whole run: the first one, kind "run"."""
        found = [event for event in self.events if event.get("kind") == "run"]
        if not found:
            raise PerfError(f"{self.run_dir / 'events.json'} has no run event")
        return found[0]

    @staticmethod
    def _json(path: Path) -> Any:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PerfError(f"cannot read {path}: {error}") from error


class Trends:
    """Values of the run's measurements in this and earlier runs on the same host and dataset."""

    def __init__(self, history: History | None, files: RunFiles, limit: int = DEFAULT_TREND_RUNS):
        self._history = history
        self._files = files
        self._limit = limit

    def of(self, row: dict[str, Any]) -> list[PastValue]:
        """Oldest first, this run last; empty without a history."""
        if self._history is None:
            return []
        results = self._files.results
        measurement = Measurement(row["source"], row["name"], row["metric"], row["unit"], row["better"], row["value"])
        # Runs that started before this one finished: this run and the ones before it.
        found = self._history.past(measurement, results["host"], results["dataset"], self._files.run["finished"])
        return list(reversed(found[:self._limit]))


@dataclass(frozen=True)
class ReportContext:
    files: RunFiles
    trends: Trends


class Reporter:
    """Base of reporter extensions. `item` is the reporter's own catalog item."""

    def __init__(self, item: Item):
        self.item = item
        self.options: dict[str, Any] = item.manifest.get("options", {})

    def render(self, context: ReportContext) -> list[Path]:
        """Writes the report into the run folder and returns the files it wrote."""
        raise NotImplementedError


class ReportRunner:
    """Runs reporters over one run folder; the history is read from the results folder."""

    def __init__(self, reporters: list[Item], loader: ExtensionLoader | None = None):
        self._reporters = reporters
        self._loader = loader or ExtensionLoader()

    def render(self, run_dir: Path, results_dir: Path, log: TextIO) -> list[str]:
        """Writes every report; returns the problems, one per reporter that failed."""
        files = RunFiles.read(run_dir)
        history = History.open(results_dir) if (results_dir / HISTORY_NAME).is_file() else None
        problems = []
        try:
            context = ReportContext(files, Trends(history, files))
            for item in self._reporters:
                try:
                    written = self._loader.load(item, Reporter)(item).render(context)
                except (PerfError, OSError, KeyError, TypeError, ValueError) as error:
                    problems.append(f"{item.key} could not write its report: {error}")
                    continue
                for path in written:
                    print(f"report: {path}", file=log)
        finally:
            if history is not None:
                history.close()
        return problems
