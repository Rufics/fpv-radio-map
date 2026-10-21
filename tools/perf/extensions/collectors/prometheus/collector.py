"""Turns the stand's Prometheus series into measurements, and saves them for the reports.

Each query of the manifest's options gives one measurement per statistic, taken over the load
phase. A query that returns several series (one per pool, per container) gives one measurement
per series, named QUERY/LABEL-VALUES. The series of every query, from a minute before the load
to a little after it, go to raw/prometheus.json, which the HTML report draws.
"""
import json
from datetime import datetime, timedelta

from perfkit.errors import PerfError
from perfkit.measure import HIGHER, LOWER, CollectContext, Collection, Collector, Measurement
from perfkit.prometheus import PrometheusClient, Series, SeriesStats

RAW_NAME = "prometheus.json"
_REQUIRED = ("name", "query", "unit", "better")


class PrometheusCollector(Collector):
    def collect(self, context: CollectContext) -> Collection:
        url = context.endpoints.get("prometheus")
        if not url:
            raise PerfError(f"{self.item.key} needs the stand's Prometheus (run without --no-monitoring)")
        client = PrometheusClient(url)
        step = float(self.options.get("stepS", 5))
        started, finished = self._moment(context.started), self._moment(context.finished)
        window = (started - timedelta(seconds=float(self.options.get("padBeforeS", 60))),
                  finished + timedelta(seconds=float(self.options.get("padAfterS", 15))))
        measurements: list[Measurement] = []
        raw = []
        for query in self._queries():
            series = client.query_range(query["query"], window[0], window[1], step)
            raw.append({**query, "series": [entry.to_json() for entry in series]})
            measurements += self._measure(context.source.key, query, series, started, finished)
        if context.raw_dir is not None:
            context.raw_dir.mkdir(parents=True, exist_ok=True)
            (context.raw_dir / RAW_NAME).write_text(json.dumps({"stepS": step, "queries": raw}), encoding="utf-8")
        return Collection(measurements)

    def _queries(self) -> list[dict]:
        queries = self.options.get("queries", [])
        for query in queries:
            missing = [key for key in _REQUIRED if key not in query]
            if missing:
                raise PerfError(f"{self.item.key}: query {query.get('name', '?')} has no {', '.join(missing)}")
            if query["better"] not in (LOWER, HIGHER):
                raise PerfError(f"{self.item.key}: query {query['name']}: better must be lower or higher")
        return queries

    @staticmethod
    def _measure(source: str, query: dict, series: list[Series], started: datetime,
                 finished: datetime) -> list[Measurement]:
        found = []
        start, end = started.timestamp(), finished.timestamp()
        for entry in series:
            points = [point for point in entry.points if start <= point[0] <= end]
            if not points:
                continue
            label = entry.describe()
            name = f"{query['name']}/{label}" if len(series) > 1 and label else query["name"]
            for stat in query.get("stats", []):
                unit = f"{query['unit']}/h" if stat == "slope" else query["unit"]
                found.append(Measurement(source, name, stat, unit, query["better"],
                                         SeriesStats.of(stat, points)))
        return found

    @staticmethod
    def _moment(text: str) -> datetime:
        try:
            return datetime.fromisoformat(text)
        except ValueError as error:
            raise PerfError(f"not a time: {text!r}") from error
