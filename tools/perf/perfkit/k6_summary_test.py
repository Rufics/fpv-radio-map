import json
import shutil
import unittest

from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.google_benchmark_test import TOOL_DIR, CollectorTestCase
from perfkit.measure import CollectContext, Collector


def summary() -> dict:
    """The shape k6 hands to handleSummary, cut down to what the collector reads."""
    trend = {"avg": 10.0, "med": 8.0, "p(90)": 20.0, "p(95)": 25.0, "p(99)": 40.0}
    return {"state": {"testRunDurationMs": 61000.0}, "setup_data": {"startedMs": 1700000000000},
            "metrics": {
                "http_reqs": {"values": {"count": 600, "rate": 9.8}},
                "http_req_duration": {"values": trend},
                "http_req_failed": {"values": {"rate": 0.005}},
                "dropped_iterations": {"values": {"count": 3}},
                "http_req_duration{persona:link-editor,step:calc-path}": {"values": {**trend, "p(95)": 120.0}},
                "http_reqs{persona:link-editor,step:calc-path}": {"values": {"rate": 5.0}},
                "http_req_failed{persona:link-editor,step:calc-path}": {"values": {"rate": 0.0}},
                "job_duration{persona:coverage-planner,step:calc-coverage}": {"values": {"p(95)": 3000.0}},
                "http_req_duration{persona:coverage-planner,step:calc-coverage}": {"values": {"p(95)": 50.0}},
                "http_reqs{persona:x,step:y}": {"values": {"rate": 1.0}},
            }}


class K6SummaryCollectorTest(CollectorTestCase):
    def setUp(self):
        super().setUp()
        target = self.root / "extensions" / "collectors" / "k6-summary"
        shutil.rmtree(target)
        shutil.copytree(TOOL_DIR / "extensions" / "collectors" / "k6-summary", target,
                        ignore=shutil.ignore_patterns("__pycache__"))
        self.item = self.catalog.get("collector", "k6-summary")
        self.item.manifest["entry"] = "collector.py:K6SummaryCollector"

    def collect(self, data):
        path = self.root / "k6-summary.json"
        path.write_text(json.dumps(data) if isinstance(data, dict) else data, encoding="utf-8")
        collector = ExtensionLoader().load(self.item, Collector)(self.item)
        return collector.collect(CollectContext(self.catalog.get("scenario", "peak"), (path,), "t0", "t1"))

    def test_whole_run_and_steps(self):
        collection = self.collect(summary())
        found = {(m.name, m.metric): (m.value, m.unit, m.better) for m in collection.measurements}
        self.assertEqual(found[("all", "p95")], (25.0, "ms", "lower"))
        self.assertEqual(found[("all", "rps")], (9.8, "1/s", "higher"))
        self.assertEqual(found[("all", "error_rate")], (0.5, "%", "lower"))
        self.assertEqual(found[("all", "dropped")], (3.0, "iterations", "lower"))
        self.assertEqual(found[("link-editor/calc-path", "p95")][0], 120.0)
        self.assertEqual(found[("link-editor/calc-path", "error_rate")][0], 0.0)
        self.assertEqual(found[("coverage-planner/calc-coverage", "job_p95")][0], 3000.0)
        self.assertNotIn(("x/y", "rps"), found)
        self.assertEqual({m.source for m in collection.measurements}, {"scenario/peak"})
        self.assertEqual(collection.context, {"k6": {"testRunDurationMs": 61000.0}})

    def test_errors(self):
        with self.assertRaisesRegex(PerfError, "no http_reqs"):
            self.collect({"metrics": {}})
        with self.assertRaisesRegex(PerfError, "cannot read the k6 summary"):
            self.collect("{")


if __name__ == "__main__":
    unittest.main()
