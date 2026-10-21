"""Profiles on a regression: what to profile, the profiler extensions, and how they are run.

When a run has a gated regression, the session asks the suite what to profile: the micro suite
runs each regressed benchmark once more under the profiler, a scenario brings the stand up again
and profiles the server under the scenario's peak load. A profiler is an extension
(extensions/profilers/<id>/) whose entry class derives from Profiler; it writes its files into
raw/ of the run, and the reports link them.
"""
import re
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

from perfkit.catalog import Item
from perfkit.commands import CommandRunner
from perfkit.compare import Comparison, Verdict
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader

# Profiling is slow; a run with many regressions profiles only the first ones.
MAX_TARGETS = 5
_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")


@dataclass(frozen=True)
class ProfileTarget:
    """A command to run under the profiler, or a running process to attach to for `duration_s`."""

    name: str
    command: tuple[str, ...] = ()
    pid: int = 0
    duration_s: float = 0.0

    def __post_init__(self):
        if bool(self.command) == bool(self.pid):
            raise PerfError(f"profile target {self.name}: give a command or a pid, not both or neither")
        if self.pid and self.duration_s <= 0:
            raise PerfError(f"profile target {self.name}: attaching to a process needs a duration")

    @property
    def file_name(self) -> str:
        """The name as a safe file name: BM_Path/100 becomes BM_Path-100."""
        return _UNSAFE.sub("-", self.name).strip("-") or "target"


@dataclass(frozen=True)
class Profile:
    """What one profiler made of one target: the files in the run folder, or why it could not."""

    profiler: str
    target: str
    files: tuple[str, ...] = ()
    error: str = ""

    def to_json(self) -> dict[str, Any]:
        return {"profiler": self.profiler, "target": self.target, "files": list(self.files), "error": self.error}


class Profiler:
    """Base of profiler extensions. `item` is the profiler's own catalog item."""

    def __init__(self, item: Item, runner: CommandRunner):
        self.item = item
        self.runner = runner
        self.options: dict[str, Any] = item.manifest.get("options", {})

    def profile(self, target: ProfileTarget, out_dir: Path) -> list[Path]:
        """Profiles `target`, writes its files into `out_dir` and returns them."""
        raise NotImplementedError


class ProfileRunner:
    """Runs every profiler over the targets a suite names; a failing profiler is noted, not fatal."""

    def __init__(self, profilers: list[Item], runner: CommandRunner, log: TextIO,
                 loader: ExtensionLoader | None = None):
        self._profilers = profilers
        self._runner = runner
        self._log = log
        self._loader = loader or ExtensionLoader()

    def __bool__(self) -> bool:
        return bool(self._profilers)

    def profile(self, targets: Sequence[ProfileTarget], run_dir: Path) -> list[Profile]:
        out_dir = run_dir / "raw"
        out_dir.mkdir(parents=True, exist_ok=True)
        done = []
        for item in self._profilers:
            for target in targets:
                done.append(self._one(item, target, run_dir, out_dir))
        return done

    def _one(self, item: Item, target: ProfileTarget, run_dir: Path, out_dir: Path) -> Profile:
        try:
            profiler = self._loader.load(item, Profiler)(item, self._runner)
            written = profiler.profile(target, out_dir)
        except (PerfError, OSError) as error:
            print(f"profile: {item.key} could not profile {target.name}: {error}", file=self._log)
            return Profile(item.key, target.name, error=str(error))
        files = tuple(path.relative_to(run_dir).as_posix() for path in written)
        for name in files:
            print(f"profile: {run_dir / name}", file=self._log)
        return Profile(item.key, target.name, files)

    @staticmethod
    def regressions(comparisons: Sequence[Comparison]) -> list[Comparison]:
        """The gated regressions, worst first."""
        found = [c for c in comparisons if c.gated and c.verdict is Verdict.REGRESSION]
        return sorted(found, key=lambda c: -(c.worse_pct or 0.0))
