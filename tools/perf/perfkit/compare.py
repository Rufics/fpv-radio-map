"""Compares a run with its baseline and decides whether a measurement regressed.

The baseline of a measurement is the median of its last `window` values from runs on the same
host and dataset, made with the same major version of every item involved (the benchmark or
scenario, and the collector). A change is the move away from that median in the bad direction,
in percent; above the limit it is a regression. With fewer than `min_runs` earlier values there
is no verdict yet.
"""
import statistics
from dataclasses import dataclass
from enum import Enum

from perfkit.history import History
from perfkit.measure import LOWER, Measurement


class Verdict(Enum):
    OK = "ok"
    REGRESSION = "regression"
    IMPROVEMENT = "improvement"
    NO_BASELINE = "no baseline"


@dataclass(frozen=True)
class Comparison:
    measurement: Measurement
    baseline: float | None
    runs: int
    worse_pct: float | None
    limit_pct: float
    gated: bool
    verdict: Verdict


@dataclass(frozen=True)
class Baseline:
    """Where a run compares: the history, the host and dataset it ran on and its start time."""

    history: History
    host: str
    dataset: str
    started: str


class Comparer:
    def __init__(self, baseline: Baseline, window: int = 10, min_runs: int = 3):
        self._baseline = baseline
        self._window = window
        self._min_runs = min_runs

    def compare(self, measurement: Measurement, involved: dict[str, str], limit_pct: float,
                gated: bool) -> Comparison:
        """`involved`: item key to version for the items whose major must match."""
        base = self._baseline
        past = [value.value for value in base.history.past(measurement, base.host, base.dataset, base.started)
                if History.same_majors(value.versions, involved)][:self._window]
        if len(past) < self._min_runs:
            return Comparison(measurement, None, len(past), None, limit_pct, gated, Verdict.NO_BASELINE)
        median = statistics.median(past)
        worse = self.worse_pct(measurement.value, median, measurement.better)
        if worse > limit_pct:
            verdict = Verdict.REGRESSION
        elif worse < -limit_pct:
            verdict = Verdict.IMPROVEMENT
        else:
            verdict = Verdict.OK
        return Comparison(measurement, median, len(past), worse, limit_pct, gated, verdict)

    @staticmethod
    def worse_pct(value: float, baseline: float, better: str) -> float:
        """How much worse `value` is than `baseline`, in percent; negative when it is better."""
        if baseline == 0 and value == 0:
            return 0.0
        if baseline == 0:
            return 100.0 if better == LOWER else -100.0
        change = (value - baseline) / abs(baseline) * 100.0
        return change if better == LOWER else -change
