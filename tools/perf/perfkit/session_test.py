import io
import json
import tempfile
import unittest
from datetime import UTC, datetime, timedelta
from pathlib import Path

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.compare import Comparison, Verdict
from perfkit.environment import Environment
from perfkit.environment_test import StaticProbe
from perfkit.errors import PerfError
from perfkit.google_benchmark_test import CatalogWithCollector
from perfkit.measure import LOWER, Collection, Measurement
from perfkit.session import Collected, ConsoleTable, Quantity, RunSession, SessionSettings


class SteppingClock:
    def __init__(self):
        self.now = datetime(2026, 1, 1, tzinfo=UTC)

    def __call__(self) -> str:
        self.now += timedelta(seconds=1)
        return self.now.isoformat(timespec="milliseconds").replace("+00:00", "Z")


class FakeSuite:
    name = "micro"
    dataset = "none"

    def __init__(self, catalog, values: dict[str, float], fail: bool = False):
        self._source = catalog.get("benchmark", "rf-core")
        self._collector = catalog.get("collector", "google-benchmark")
        self.values = values
        self.fail = fail

    def items(self):
        return [self._source, self._collector]

    def environment(self):
        return {"server": {"threads": 16}}

    def problems(self):
        return []

    def execute(self, timeline, run_dir):
        with timeline.phase("benchmark/rf-core", "benchmark"):
            if self.fail:
                raise PerfError("binary crashed")
        measurements = [Measurement("benchmark/rf-core", name, metric, "ns", LOWER, value)
                        for name, value in self.values.items() for metric in ("cpu_time", "real_time")]
        collection = Collection(measurements, {"googleBenchmark": {"num_cpus": 4}})
        return [Collected(self._source, self._collector, collection, frozenset({"cpu_time"}), 5.0)]


class RunSessionTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        CatalogWithCollector(self.root / "catalog")
        self.catalog, _ = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root / "catalog")
        self.clock = SteppingClock()
        self.environment = Environment([StaticProbe("host", {"cpuModel": "X"}), StaticProbe("git", {"commit": "c1"})])

    def run_suite(self, values, gate=True, fail=False):
        output = io.StringIO()
        session = RunSession(SessionSettings(self.root / "results", gate=gate), self.environment, output, self.clock)
        return session.run(FakeSuite(self.catalog, values, fail)), output.getvalue()

    def test_first_run_writes_every_file(self):
        code, output = self.run_suite({"BM_A": 100.0})
        self.assertEqual(code, 0)
        run_dir = self.root / "results" / "20260101T000001Z"
        self.assertIn("no baseline", output)
        self.assertIn(f"results in {run_dir}", output)
        results = json.loads((run_dir / "results.json").read_text(encoding="utf-8"))
        self.assertEqual(results["items"], {"benchmark/rf-core": "1.0.0", "collector/google-benchmark": "1.0.0"})
        self.assertEqual(len(results["results"]), 2)
        self.assertEqual(results["results"][0]["verdict"], "no baseline")
        environment = json.loads((run_dir / "environment.json").read_text(encoding="utf-8"))
        self.assertEqual(environment["tools"], {"googleBenchmark": {"num_cpus": 4}})
        events = json.loads((run_dir / "events.json").read_text(encoding="utf-8"))
        self.assertEqual([e["name"] for e in events], ["micro run", "benchmark/rf-core"])
        self.assertTrue((self.root / "results" / "history.sqlite").is_file())

    def test_regression_fails_the_gate_unless_disabled(self):
        for _ in range(3):
            self.run_suite({"BM_A": 100.0})
        self.assertEqual(self.run_suite({"BM_A": 104.0})[0], 0)
        code, output = self.run_suite({"BM_A": 120.0})
        self.assertEqual(code, 1)
        self.assertIn("1 regression(s)", output)
        self.assertEqual(self.run_suite({"BM_A": 120.0}, gate=False)[0], 0)

    def test_failed_run_keeps_its_events(self):
        with self.assertRaisesRegex(PerfError, "crashed"):
            self.run_suite({}, fail=True)
        run_dir = next((self.root / "results").iterdir())
        events = json.loads((run_dir / "events.json").read_text(encoding="utf-8"))
        self.assertEqual([e["outcome"] for e in events], ["failed", "failed"])

    def test_run_folders_never_clash(self):
        fixed = RunSession(SessionSettings(self.root / "results"), self.environment, io.StringIO(),
                           lambda: "2026-01-01T00:00:00.000Z")
        first = fixed._run_dir()
        second = fixed._run_dir()
        self.assertEqual((first.name, second.name), ("20260101T000000Z", "20260101T000000Z-2"))
        for attempt in range(3, 100):
            (self.root / "results" / f"20260101T000000Z-{attempt}").mkdir()
        with self.assertRaisesRegex(PerfError, "too many runs"):
            fixed._run_dir()


class ConsoleTableTest(unittest.TestCase):
    def test_worst_first_and_units(self):
        def comparison(name, value, worse, verdict):
            measurement = Measurement("benchmark/rf-core", name, "cpu_time", "ns", LOWER, value)
            return Comparison(measurement, 1500.0, 5, worse, 5.0, True, verdict)
        output = io.StringIO()
        ConsoleTable(output).print(Path("/r"), [comparison("BM_A", 1500.0, 0.0, Verdict.OK),
                                                comparison("BM_B", 2.5e6, 9.0, Verdict.REGRESSION)])
        lines = output.getvalue().splitlines()
        self.assertTrue(lines[1].startswith("benchmark/rf-core  BM_B"))
        self.assertIn("2.5 ms", lines[1])
        self.assertIn("+9.0%", lines[1])
        self.assertIn("1.5 µs", lines[2])
        self.assertEqual(lines[3], "2 gated measurement(s), 1 regression(s); results in /r")

    def test_quantity(self):
        self.assertEqual(Quantity.text(None, "ns"), "-")
        self.assertEqual(Quantity.text(7.5, "ns"), "7.5 ns")
        self.assertEqual(Quantity.text(3.2e9, "ns"), "3.2 s")
        self.assertEqual(Quantity.text(4.0e5, "1/s"), "4e+05 1/s")


if __name__ == "__main__":
    unittest.main()
