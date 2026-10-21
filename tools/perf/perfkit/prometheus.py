"""Reads time series from the stand's Prometheus (HTTP API v1, query_range).

Collectors use it to turn a series into numbers (the maximum, the mean, the slope) and reports
to draw it. Values Prometheus cannot compute come back as "NaN" and are left out.
"""
import math
import urllib.parse
from dataclasses import dataclass, field
from datetime import datetime

from perfkit.api import ApiClient
from perfkit.errors import PerfError

SECONDS_PER_HOUR = 3600.0


# Labels of the metric name and the scrape target: the same in every run, so not part of a name.
_TARGET_LABELS = frozenset({"__name__", "instance", "job"})


@dataclass(frozen=True)
class Series:
    labels: dict[str, str]
    points: list[tuple[float, float]] = field(default_factory=list)  # (unix seconds, value)

    def describe(self) -> str:
        """The label values that tell this series from the others of its query, e.g. "http"."""
        return ",".join(value for name, value in sorted(self.labels.items()) if name not in _TARGET_LABELS)

    def to_json(self) -> dict:
        return {"labels": self.labels, "values": [[time, value] for time, value in self.points]}


class SeriesStats:
    """The numbers a collector can make of one series."""

    NAMES = ("max", "min", "avg", "last", "slope", "tail_slope")

    @classmethod
    def of(cls, stat: str, points: list[tuple[float, float]]) -> float:
        if not points:
            raise PerfError("the series has no values")
        values = [value for _, value in points]
        if stat == "max":
            return max(values)
        if stat == "min":
            return min(values)
        if stat == "avg":
            return sum(values) / len(values)
        if stat == "last":
            return values[-1]
        if stat == "slope":
            return cls.slope(points)
        if stat == "tail_slope":
            # The second half only: caches fill up early in a run and would read as a leak.
            return cls.slope(points[len(points) // 2:])
        raise PerfError(f"unknown statistic {stat}; one of {', '.join(cls.NAMES)}")

    @staticmethod
    def slope(points: list[tuple[float, float]]) -> float:
        """Least-squares growth per hour: a leak shows as a steady positive slope."""
        if len(points) < 2:
            return 0.0
        mean_t = sum(time for time, _ in points) / len(points)
        mean_v = sum(value for _, value in points) / len(points)
        spread = sum((time - mean_t) ** 2 for time, _ in points)
        if spread == 0:
            return 0.0
        return sum((time - mean_t) * (value - mean_v) for time, value in points) / spread * SECONDS_PER_HOUR


class PrometheusClient:
    def __init__(self, base_url: str, client: ApiClient | None = None):
        self._client = client or ApiClient(base_url)

    def query_range(self, query: str, start: datetime, end: datetime, step_s: float) -> list[Series]:
        params = urllib.parse.urlencode({"query": query, "start": f"{start.timestamp():.3f}",
                                         "end": f"{end.timestamp():.3f}", "step": f"{step_s:g}"})
        reply = self._client.call("GET", f"/query_range?{params}")
        body = reply.body if isinstance(reply.body, dict) else {}
        if reply.status != 200 or body.get("status") != "success":
            raise PerfError(f"Prometheus could not run {query!r}: {body.get('error', reply.body)}")
        return [self._series(entry) for entry in body["data"]["result"]]

    @staticmethod
    def _series(entry: dict) -> Series:
        points = []
        for time, raw in entry.get("values", []):
            value = float(raw)
            if math.isfinite(value):
                points.append((float(time), value))
        return Series(dict(entry.get("metric", {})), points)
