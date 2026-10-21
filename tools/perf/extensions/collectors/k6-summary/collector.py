"""The JSON k6/runner.js writes from handleSummary into measurements.

Measurement names: "all" for the whole run, "PERSONA/STEP" for one step of one persona (the
runner keeps those sub-metrics with always-true thresholds). Latencies in milliseconds.
"""
import json
import re

from perfkit.errors import PerfError
from perfkit.measure import HIGHER, LOWER, CollectContext, Collection, Collector, Measurement

_TAGGED = re.compile(r"^(?P<metric>[a-z_]+)\{persona:(?P<persona>[a-z0-9-]+),step:(?P<step>[a-z0-9-]+)\}$")
_LATENCIES = {"p95": "p(95)", "p99": "p(99)", "med": "med"}


class K6SummaryCollector(Collector):
    def collect(self, context: CollectContext) -> Collection:
        data = self._read(context.files[0])
        metrics = data.get("metrics", {})
        if "http_reqs" not in metrics:
            raise PerfError(f"{context.files[0]} has no http_reqs: did k6 send any request?")
        source = context.source.key
        found = self._group(source, "all", metrics, "")
        found.append(Measurement(source, "all", "dropped", "iterations", LOWER,
                                 float(metrics.get("dropped_iterations", {}).get("values", {}).get("count", 0))))
        for name in sorted(metrics):
            match = _TAGGED.match(name)
            if match and match["metric"] == "http_req_duration":
                tags = name[len("http_req_duration"):]
                found.extend(self._group(source, f"{match['persona']}/{match['step']}", metrics, tags))
        return Collection(found, {"k6": {"testRunDurationMs": data.get("state", {}).get("testRunDurationMs")}})

    @staticmethod
    def _read(path) -> dict:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PerfError(f"cannot read the k6 summary {path}: {error}") from error

    @staticmethod
    def _group(source: str, name: str, metrics: dict, tags: str) -> list[Measurement]:
        found = []
        duration = metrics.get(f"http_req_duration{tags}", {}).get("values", {})
        found += [Measurement(source, name, metric, "ms", LOWER, float(duration[key]))
                  for metric, key in _LATENCIES.items() if key in duration]
        rate = metrics.get(f"http_reqs{tags}", {}).get("values", {})
        if "rate" in rate:
            found.append(Measurement(source, name, "rps", "1/s", HIGHER, float(rate["rate"])))
        failed = metrics.get(f"http_req_failed{tags}", {}).get("values", {})
        if "rate" in failed:
            found.append(Measurement(source, name, "error_rate", "%", LOWER, float(failed["rate"]) * 100.0))
        job = metrics.get(f"job_duration{tags}", {}).get("values", {})
        if "p(95)" in job:
            found.append(Measurement(source, name, "job_p95", "ms", LOWER, float(job["p(95)"])))
        return found
