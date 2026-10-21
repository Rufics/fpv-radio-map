import json
import shutil
import tempfile
import unittest
from pathlib import Path

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR, SampleCatalog
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.measure import CollectContext, Collector

TOOL_DIR = Path(__file__).resolve().parent.parent
COLLECTOR_DIR = TOOL_DIR / "extensions" / "collectors" / "google-benchmark"


def aggregate(name: str, stat: str, real: float, cpu: float, unit: str = "ns", **extra) -> dict:
    return {"name": f"{name}_{stat}", "run_name": name, "run_type": "aggregate", "aggregate_name": stat,
            "real_time": real, "cpu_time": cpu, "time_unit": unit, **extra}


def benchmark_output(benchmarks: list[dict] | None = None) -> dict:
    """What a binary writes with --benchmark_repetitions and aggregates only."""
    if benchmarks is None:
        benchmarks = [aggregate("BM_A", "mean", 12.0, 11.0), aggregate("BM_A", "median", 10.0, 9.0),
                      aggregate("BM_B/400", "median", 2.5, 2.0, "us", items_per_second=4.0e5),
                      {"name": "BM_B_BigO", "run_name": "BM_B", "run_type": "aggregate", "aggregate_name": "BigO",
                       "cpu_coefficient": 1.0, "real_coefficient": 1.0, "big_o": "N", "time_unit": "ns"}]
    return {"context": {"host_name": "vm", "num_cpus": 4, "library_build_type": "release", "executable": "x"},
            "benchmarks": benchmarks}


class CatalogWithCollector(SampleCatalog):
    """The sample catalog plus the real google-benchmark collector of the repository."""

    def __init__(self, root: Path):
        super().__init__(root)
        shutil.copytree(COLLECTOR_DIR, root / "extensions" / "collectors" / "google-benchmark",
                        ignore=shutil.ignore_patterns("__pycache__"))


class CollectorTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.sample = CatalogWithCollector(self.root)
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root)
        self.assertEqual(problems, [])


class GoogleBenchmarkCollectorTest(CollectorTestCase):
    def collect(self, output, benchmark_manifest=None):
        path = self.root / "out.json"
        path.write_text(output if isinstance(output, str) else json.dumps(output), encoding="utf-8")
        item = self.catalog.get("collector", "google-benchmark")
        collector = ExtensionLoader().load(item, Collector)(item)
        source = self.catalog.get("benchmark", "rf-core")
        if benchmark_manifest:
            source.manifest.update(benchmark_manifest)
        return collector.collect(CollectContext(source, (path,), "t0", "t1"))

    def test_median_aggregates_in_nanoseconds(self):
        collection = self.collect(benchmark_output())
        found = {(m.name, m.metric): (m.value, m.unit, m.better) for m in collection.measurements}
        self.assertEqual(found, {
            ("BM_A", "real_time"): (10.0, "ns", "lower"), ("BM_A", "cpu_time"): (9.0, "ns", "lower"),
            ("BM_B/400", "real_time"): (2500.0, "ns", "lower"), ("BM_B/400", "cpu_time"): (2000.0, "ns", "lower"),
            ("BM_B/400", "items_per_second"): (4.0e5, "1/s", "higher")})
        self.assertEqual(collection.measurements[0].source, "benchmark/rf-core")
        self.assertEqual(collection.context["googleBenchmark"]["library_build_type"], "release")
        self.assertNotIn("executable", collection.context["googleBenchmark"])

    def test_stat_follows_the_benchmark(self):
        collection = self.collect(benchmark_output(), {"compare": {"stat": "mean"}})
        self.assertEqual([m.value for m in collection.measurements], [12.0, 11.0])

    def test_errors(self):
        failed = aggregate("BM_A", "median", 1, 1, error_occurred=True, error_message="no data")
        cases = {"failed: no data": benchmark_output([failed]),
                 "unknown time unit": benchmark_output([aggregate("BM_A", "median", 1, 1, "min")]),
                 "no 'median' aggregates": benchmark_output([]),
                 "cannot read benchmark output": "{"}
        for message, output in cases.items():
            with self.subTest(message=message), self.assertRaisesRegex(PerfError, message):
                self.collect(output)


class ExtensionLoaderTest(CollectorTestCase):
    def collector(self, entry: str, code: str):
        folder = self.root / "extensions" / "collectors" / "k6-summary"
        (folder / "collector.py").write_text(code, encoding="utf-8")
        item = self.catalog.get("collector", "k6-summary")
        item.manifest["entry"] = entry
        return item

    def test_loads_once(self):
        loader = ExtensionLoader()
        item = self.catalog.get("collector", "google-benchmark")
        self.assertIs(loader.load(item, Collector), loader.load(item, Collector))

    def test_bad_entries(self):
        cases = [("collector.py:Missing", "class Other:\n    pass\n", "has no class Missing"),
                 ("collector.py:K6SummaryCollector", "class K6SummaryCollector:\n    pass\n", "is not a Collector"),
                 ("collector.py:K6SummaryCollector", "def broken(:\n", "cannot import"),
                 ("collector.py:K6SummaryCollector", "import perfkit_no_such_module\n", "cannot import")]
        for entry, code, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(PerfError, message):
                ExtensionLoader().load(self.collector(entry, code), Collector)

    def test_base_collector_is_abstract(self):
        with self.assertRaises(NotImplementedError):
            Collector(self.catalog.get("collector", "k6-summary")).collect(None)


if __name__ == "__main__":
    unittest.main()
