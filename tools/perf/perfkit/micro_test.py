import json
import unittest
from pathlib import Path

from perfkit.catalog import Catalog, CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.errors import PerfError
from perfkit.google_benchmark_test import CollectorTestCase, benchmark_output
from perfkit.micro import MicroBuild, MicroOptions, MicroSuite
from perfkit.timeline import Timeline


class FakeRunner:
    """Records the commands; a benchmark binary 'writes' the sample output."""

    def __init__(self, output: dict | None = None):
        self.commands: list[list[str]] = []
        self.output = output or benchmark_output()

    def run(self, args, cwd=None):
        self.commands.append(args)
        for arg in args:
            if arg.startswith("--benchmark_out="):
                Path(arg.split("=", 1)[1]).write_text(json.dumps(self.output), encoding="utf-8")


class MicroSuiteTest(CollectorTestCase):
    def setUp(self):
        super().setUp()
        self.sample.write("extensions/benchmarks/result-cache", {
            "kind": "benchmark", "id": "result-cache", "version": "2.1.0", "target": "bench_result_cache",
            "links": ["rfmap_server_lib"], "repetitions": 4, "collector": "google-benchmark",
            "compare": {"metric": "real_time", "maxRegressionPct": 3}}, {"CMakeLists.txt": "rfmap_perf_benchmark()\n"})
        self.reload()
        self.runner = FakeRunner()
        self.build = MicroBuild(Path("/src"), self.root / "build", self.root)

    def reload(self):
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root)
        self.assertEqual(problems, [])

    def suite(self, **options):
        return MicroSuite(self.catalog, self.build, self.runner, MicroOptions(**options))

    def test_builds_once_and_runs_every_benchmark(self):
        collected = self.suite().execute(Timeline(), self.root / "run")
        configure, build, first, second = self.runner.commands
        self.assertIn("-DRFMAP_BUILD_BENCHMARKS=ON", configure)
        self.assertIn(f"-DRFMAP_PERF_CATALOG_DIR={self.root}", configure)
        self.assertEqual(build[-2:], ["bench_result_cache", "bench_rf_core"])
        self.assertEqual(first[0], str(self.root / "build" / "perf" / "bin" / "bench_result_cache"))
        self.assertIn("--benchmark_repetitions=4", first)
        self.assertIn("--benchmark_repetitions=10", second)
        cache, core = collected
        self.assertEqual((cache.gate_metrics, cache.limit_pct), (frozenset({"real_time"}), 3.0))
        self.assertEqual((core.gate_metrics, core.limit_pct), (frozenset({"cpu_time"}), 5.0))
        self.assertEqual(core.involved, {"benchmark/rf-core": "1.0.0", "collector/google-benchmark": "1.0.0"})
        self.assertTrue((self.root / "run" / "raw" / "rf-core.json").is_file())

    def test_only_and_repetitions(self):
        self.suite(only=("rf-core",), repetitions=2).execute(Timeline(), self.root / "run")
        self.assertEqual(self.runner.commands[1][-1], "bench_rf_core")
        self.assertIn("--benchmark_repetitions=2", self.runner.commands[2])

    def test_items_include_the_collectors_once(self):
        keys = [item.key for item in self.suite().items()]
        self.assertEqual(keys, ["benchmark/result-cache", "benchmark/rf-core", "collector/google-benchmark"])

    def test_unknown_benchmark(self):
        with self.assertRaisesRegex(PerfError, "no benchmark nope"):
            self.suite(only=("nope", "rf-core")).items()

    def test_missing_collector(self):
        self.catalog.get("benchmark", "rf-core").manifest["collector"] = "gone"
        with self.assertRaisesRegex(PerfError, "no collector 'gone'"):
            self.suite(only=("rf-core",)).items()

    def test_no_benchmarks(self):
        suite = MicroSuite(Catalog(self.root, []), self.build, self.runner)
        with self.assertRaisesRegex(PerfError, "no benchmarks"):
            suite.execute(Timeline(), self.root / "run")
        self.assertEqual(self.runner.commands, [])


if __name__ == "__main__":
    unittest.main()
