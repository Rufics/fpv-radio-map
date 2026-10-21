import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from perfkit import cli
from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.environment import Environment
from perfkit.environment_test import StaticProbe
from perfkit.errors import PerfError
from perfkit.extensions import ExtensionLoader
from perfkit.google_benchmark_test import TOOL_DIR, CatalogWithCollector
from perfkit.history import PastValue
from perfkit.report import Reporter, ReportRunner, RunFiles
from perfkit.session import RunSession, SessionSettings
from perfkit.session_test import FakeSuite, SteppingClock

REPORTERS_DIR = TOOL_DIR / "extensions" / "reporters"


class CatalogWithReporters(CatalogWithCollector):
    """The sample catalog with the repository's real html and markdown reporters."""

    def __init__(self, root: Path):
        super().__init__(root)
        shutil.rmtree(root / "extensions" / "reporters")
        shutil.copytree(REPORTERS_DIR, root / "extensions" / "reporters", ignore=shutil.ignore_patterns("__pycache__"))


class ReportTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        CatalogWithReporters(self.root / "catalog")
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root / "catalog")
        self.assertEqual(problems, [])
        self.clock = SteppingClock()
        self.environment = Environment([StaticProbe("host", {"cpuModel": "X | Y", "logicalCpus": 4}),
                                        StaticProbe("git", {"commit": "c0ffee", "dirty": True})])
        self.results = self.root / "results"

    def run_suite(self, values: dict[str, float]) -> tuple[int, str, Path]:
        output = io.StringIO()
        runner = ReportRunner(self.catalog.items("reporter"))
        session = RunSession(SessionSettings(self.results), self.environment, output, self.clock, reports=runner)
        code = session.run(FakeSuite(self.catalog, values))
        return code, output.getvalue(), max(path for path in self.results.iterdir() if path.is_dir())


class ReportsOfARunTest(ReportTestCase):
    def test_every_run_gets_both_reports(self):
        for _ in range(3):
            self.run_suite({"BM_A": 100.0, "BM_</script>": 5.0})
        code, output, run_dir = self.run_suite({"BM_A": 130.0, "BM_</script>": 5.0})
        self.assertEqual(code, 1)
        self.assertIn(f"report: {run_dir / 'report.html'}", output)
        page = (run_dir / "report.html").read_text(encoding="utf-8")
        self.assertIn('integrity="sha384-', page)
        self.assertNotIn("BM_</script>", page, "names are escaped, also inside the figures")
        figures = json.loads(page.split("const figures = ", 1)[1].split(";\n", 1)[0].replace("<\\/", "</"))
        trend = next(figure for figure in figures.values()
                     if "BM_A" in figure["layout"].get("title", {}).get("text", ""))
        self.assertEqual(len(trend["data"][0]["y"]), 4)
        self.assertEqual(trend["data"][1]["marker"]["color"], "#c62828")
        self.assertIn("regression", page)
        summary = (run_dir / "summary.md").read_text(encoding="utf-8")
        self.assertTrue(summary.startswith("### 🔴 1 regression(s) in `micro`"))
        self.assertIn("| 🔴 | `BM_A` cpu_time | 130 ns | 100 ns | +30.0% | 5% |", summary)
        self.assertIn("X / Y × 4", summary)
        self.assertIn("`c0ffee (dirty)`", summary)

    def test_first_run_has_no_baseline_and_no_series(self):
        _, _, run_dir = self.run_suite({"BM_A": 100.0})
        summary = (run_dir / "summary.md").read_text(encoding="utf-8")
        self.assertTrue(summary.startswith("### ⚫ No baseline yet for"))
        page = (run_dir / "report.html").read_text(encoding="utf-8")
        self.assertNotIn("Server, database and load generator", page)
        self.assertIn("<h2>Timeline</h2>", page)

    def test_prometheus_series_with_bands(self):
        _, _, run_dir = self.run_suite({"BM_A": 100.0})
        events = json.loads((run_dir / "events.json").read_text(encoding="utf-8"))
        events.append({"name": "steady", "kind": "stage", "started": "2026-01-01T00:00:01.000Z",
                       "finished": "2026-01-01T00:01:01.000Z", "outcome": "", "attributes": {}})
        (run_dir / "events.json").write_text(json.dumps(events), encoding="utf-8")
        (run_dir / "raw").mkdir()
        raw = {"stepS": 5, "queries": [
            {"name": "pool_queued", "title": "Queued", "unit": "tasks", "query": "q",
             "series": [{"labels": {"pool": "http", "job": "x"},
                         "values": [[1767225601.0, 1.0], [1767225606.0, 2.0]]}]},
            {"name": "db_memory", "title": "DB", "unit": "MiB", "query": "m", "series": []}]}
        (run_dir / "raw" / "prometheus.json").write_text(json.dumps(raw), encoding="utf-8")
        self.assertEqual(ReportRunner(self.catalog.items("reporter")).render(run_dir, self.results, io.StringIO()), [])
        page = (run_dir / "report.html").read_text(encoding="utf-8")
        self.assertIn("No data for: db_memory.", page)
        figures = json.loads(page.split("const figures = ", 1)[1].split(";\n", 1)[0])
        queued = next(f for f in figures.values() if f["layout"].get("title", {}).get("text") == "Queued")
        self.assertEqual(queued["data"][0]["name"], "http")
        self.assertEqual(queued["data"][0]["x"][0], "2026-01-01 00:00:01.000")
        self.assertIn({"x0": "2026-01-01 00:00:01.000", "x1": "2026-01-01 00:01:01.000"},
                      [{k: shape[k] for k in ("x0", "x1")} for shape in queued["layout"]["shapes"]])
        self.assertEqual([label["text"] for label in queued["layout"]["annotations"]], ["benchmark/rf-core", "steady"])


class TrendChartTest(ReportTestCase):
    def test_version_changes_are_marked(self):
        reporter = ExtensionLoader().load(self.catalog.get("reporter", "html"), Reporter)
        module = __import__(reporter.__module__, fromlist=["TrendChart", "Figures"])
        row = {"name": "BM_A", "metric": "cpu_time", "unit": "ns", "better": "lower", "baseline": 2000.0,
               "limitPct": 5.0, "verdict": "ok"}
        points = [PastValue("r1", "2026-01-01T00:00:00Z", 2000.0, {"benchmark/rf-core": "1.0.0"}),
                  PastValue("r2", "2026-01-02T00:00:00Z", 2100.0, {"benchmark/rf-core": "1.1.0"})]
        figures = module.Figures()
        module.TrendChart(row, points).add_to(figures)
        (figure,) = figures.figures.values()
        self.assertEqual(figure["layout"]["yaxis"]["title"]["text"], "µs")
        self.assertEqual(figure["data"][0]["y"], [2.0, 2.1])
        self.assertEqual(figure["layout"]["annotations"][0]["hovertext"], "benchmark/rf-core 1.0.0 → 1.1.0")
        baseline, allowed = figure["layout"]["shapes"][:2]
        self.assertEqual(baseline["y0"], 2.0)
        self.assertAlmostEqual(allowed["y1"], 2.1)
        self.assertEqual(module.TrendChart(row, []).add_to(figures), "")


class ReportRunnerTest(ReportTestCase):
    def test_a_failing_reporter_is_a_problem_not_a_crash(self):
        _, _, run_dir = self.run_suite({"BM_A": 100.0})
        (run_dir / "results.json").write_text(json.dumps({"runId": "x", "suite": "micro", "host": "h",
                                                          "dataset": "none", "results": [{"gated": True}]}))
        problems = ReportRunner(self.catalog.items("reporter")).render(run_dir, self.results, io.StringIO())
        self.assertEqual(len(problems), 2)
        self.assertIn("reporter/html could not write its report", problems[0])

    def test_unreadable_run_folder(self):
        with self.assertRaisesRegex(PerfError, "cannot read"):
            RunFiles.read(self.root / "nope")

    def test_report_command(self):
        _, _, run_dir = self.run_suite({"BM_A": 100.0})
        (run_dir / "summary.md").unlink()
        output, errors = io.StringIO(), io.StringIO()
        main = cli.PerfCli(output=output, errors=errors).main
        catalog = ["--catalog-dir", str(self.root / "catalog")]
        self.assertEqual(main([*catalog, "report", str(run_dir), "--reporter", "markdown"]), 0, errors.getvalue())
        self.assertTrue((run_dir / "summary.md").is_file())
        self.assertEqual(output.getvalue().count("report: "), 1)
        self.assertEqual(main([*catalog, "report", str(run_dir), "--reporter", "pdf"]), 1)
        self.assertIn("no reporter pdf", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
