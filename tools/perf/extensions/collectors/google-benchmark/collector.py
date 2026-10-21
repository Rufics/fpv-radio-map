"""Google Benchmark JSON (--benchmark_format=json) into measurements.

With repetitions, the binary reports every repetition and the aggregates mean, median, stddev
and cv. Only the aggregate the benchmark item asks for ("compare.stat", median by default) is
kept, with times in nanoseconds. Complexity fits (BigO, RMS) are skipped.
"""
import json

from perfkit.errors import PerfError
from perfkit.measure import HIGHER, LOWER, CollectContext, Collection, Collector, Measurement

_TO_NANOSECONDS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
_TIMES = ("real_time", "cpu_time")
_RATES = {"items_per_second": "1/s", "bytes_per_second": "B/s"}
# What the binary says about the machine and itself; it goes into environment.json.
_CONTEXT_KEYS = ("host_name", "num_cpus", "mhz_per_cpu", "cpu_scaling_enabled", "caches", "load_avg",
                 "library_version", "library_build_type")


class GoogleBenchmarkCollector(Collector):
    def collect(self, context: CollectContext) -> Collection:
        stat = context.source.manifest.get("compare", {}).get("stat", "median")
        measurements: list[Measurement] = []
        machine: dict = {}
        for path in context.files:
            data = self._read(path)
            machine = {key: data["context"][key] for key in _CONTEXT_KEYS if key in data.get("context", {})}
            for entry in data.get("benchmarks", []):
                measurements.extend(self._entry(context.source.key, entry, stat))
        if not measurements:
            raise PerfError(f"{context.source.key}: no '{stat}' aggregates; run the binary with repetitions")
        return Collection(measurements, {"googleBenchmark": machine})

    @staticmethod
    def _read(path) -> dict:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PerfError(f"cannot read benchmark output {path}: {error}") from error

    @staticmethod
    def _entry(source: str, entry: dict, stat: str) -> list[Measurement]:
        if entry.get("error_occurred"):
            raise PerfError(f"{source}: {entry.get('name')} failed: {entry.get('error_message', 'no message')}")
        if entry.get("run_type") != "aggregate" or entry.get("aggregate_name") != stat:
            return []
        name = entry["run_name"]
        scale = _TO_NANOSECONDS.get(entry.get("time_unit", "ns"))
        if scale is None:
            raise PerfError(f"{source}: {name} has an unknown time unit {entry.get('time_unit')!r}")
        found = [Measurement(source, name, metric, "ns", LOWER, float(entry[metric]) * scale)
                 for metric in _TIMES if metric in entry]
        found += [Measurement(source, name, metric, unit, HIGHER, float(entry[metric]))
                  for metric, unit in _RATES.items() if metric in entry]
        return found
