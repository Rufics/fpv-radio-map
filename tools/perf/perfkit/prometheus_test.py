import json
import shutil
import unittest
from datetime import UTC, datetime

from perfkit.api import ApiClient
from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.fake_server_test import FakeServer
from perfkit.google_benchmark_test import TOOL_DIR, CollectorTestCase
from perfkit.measure import CollectContext, Collector, Measurement
from perfkit.prometheus import PrometheusClient, Series, SeriesStats
from perfkit.session import Collected

POINTS = [(0.0, 1.0), (1800.0, 2.0), (3600.0, 3.0)]


class SeriesStatsTest(unittest.TestCase):
    def test_statistics(self):
        self.assertEqual(SeriesStats.of("max", POINTS), 3.0)
        self.assertEqual(SeriesStats.of("min", POINTS), 1.0)
        self.assertEqual(SeriesStats.of("avg", POINTS), 2.0)
        self.assertEqual(SeriesStats.of("last", POINTS), 3.0)
        self.assertAlmostEqual(SeriesStats.of("slope", POINTS), 2.0)  # per hour

    def test_flat_and_short_series_have_no_slope(self):
        self.assertEqual(SeriesStats.slope([(5.0, 1.0)]), 0.0)
        self.assertEqual(SeriesStats.slope([(5.0, 1.0), (5.0, 2.0)]), 0.0)

    def test_errors(self):
        with self.assertRaisesRegex(PerfError, "no values"):
            SeriesStats.of("max", [])
        with self.assertRaisesRegex(PerfError, "unknown statistic p42"):
            SeriesStats.of("p42", POINTS)

    def test_series_names_itself_by_its_labels(self):
        self.assertEqual(Series({"__name__": "x", "pool": "http", "a": "1"}).describe(), "1,http")
        self.assertEqual(Series({}, [(1.0, 2.0)]).to_json(), {"labels": {}, "values": [[1.0, 2.0]]})


class PrometheusCollectorTest(CollectorTestCase):
    def setUp(self):
        super().setUp()
        self.server = FakeServer()
        self.addCleanup(self.server.stop)
        target = self.root / "extensions" / "collectors" / "prometheus"
        shutil.copytree(TOOL_DIR / "extensions" / "collectors" / "prometheus", target,
                        ignore=shutil.ignore_patterns("__pycache__"))
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root)
        self.assertEqual(problems, [])
        self.item = self.catalog.get("collector", "prometheus")

    def collect(self, queries, endpoints=None):
        self.item.manifest["options"] = {"stepS": 10, "padBeforeS": 30, "padAfterS": 0, "queries": queries}
        collector = ExtensionLoader().load(self.item, Collector)(self.item)
        endpoints = {"prometheus": self.server.base_url} if endpoints is None else endpoints
        context = CollectContext(self.catalog.get("scenario", "peak"), (), "2026-10-21T10:00:00.000Z",
                                 "2026-10-21T10:01:00.000Z", endpoints, self.root / "raw")
        return collector.collect(context)

    def test_measurements_and_raw_series(self):
        collection = self.collect([
            {"name": "server_rss", "query": "rss", "unit": "MiB", "better": "lower", "stats": ["max", "slope"]},
            {"name": "pool_queued", "query": "max by (pool) (q)", "unit": "tasks", "better": "lower",
             "stats": ["avg"]},
            {"name": "k6_p95", "query": "k6", "unit": "ms", "better": "lower", "stats": []}])
        found = {(m.name, m.metric): (m.value, m.unit) for m in collection.measurements}
        # 30 s of padding is 3 steps before the load; the load has the values 4 to 10.
        self.assertEqual(found[("server_rss", "max")], (10.0, "MiB"))
        self.assertAlmostEqual(found[("server_rss", "slope")][0], 360.0)
        self.assertEqual(found[("server_rss", "slope")][1], "MiB/h")
        self.assertEqual(found[("pool_queued/http", "avg")], (7.0, "tasks"))
        self.assertIn(("pool_queued/job-runners", "avg"), found)
        self.assertEqual(len(found), 4)
        raw = json.loads((self.root / "raw" / "prometheus.json").read_text(encoding="utf-8"))
        self.assertEqual(raw["stepS"], 10)
        self.assertEqual(len(raw["queries"][0]["series"][0]["values"]), 10)

    def test_errors(self):
        with self.assertRaisesRegex(PerfError, "needs the stand's Prometheus"):
            self.collect([], endpoints={})
        with self.assertRaisesRegex(PerfError, "has no unit, better"):
            self.collect([{"name": "x", "query": "x"}])
        with self.assertRaisesRegex(PerfError, "better must be lower or higher"):
            self.collect([{"name": "x", "query": "x", "unit": "u", "better": "more"}])
        with self.assertRaisesRegex(PerfError, "could not run 'bad'"):
            self.collect([{"name": "x", "query": "bad", "unit": "u", "better": "lower"}])

    def test_client_reports_a_missing_endpoint(self):
        client = PrometheusClient("", ApiClient(self.server.base_url.replace("/api/v1", "/nope")))
        with self.assertRaisesRegex(PerfError, "could not run"):
            client.query_range("up", *_window(), 5)


class GateTest(CollectorTestCase):
    def test_a_gate_names_a_metric_or_one_measurement(self):
        peak = self.catalog.get("scenario", "peak")
        collected = Collected(peak, peak, None, frozenset({"p95", "server_rss:slope"}), 5.0)  # type: ignore[arg-type]

        def gated(name, metric):
            return collected.gates(Measurement("s", name, metric, "u", "lower", 1.0))

        self.assertTrue(gated("all", "p95"))
        self.assertTrue(gated("server_rss", "slope"))
        self.assertFalse(gated("server_rss", "max"))
        self.assertFalse(gated("db_memory", "slope"))


def _window():
    return datetime(2026, 10, 21, 10, tzinfo=UTC), datetime(2026, 10, 21, 10, 1, tzinfo=UTC)


if __name__ == "__main__":
    unittest.main()
