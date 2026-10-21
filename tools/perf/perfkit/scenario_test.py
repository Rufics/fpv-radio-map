import json
import unittest
from unittest.mock import patch

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.errors import PerfError
from perfkit.google_benchmark_test import TOOL_DIR
from perfkit.k6_summary_test import summary
from perfkit.scenario import Dataset, ScenarioSuite
from perfkit.stand import Stand, StandSettings
from perfkit.stand_test import FakeRunner, StandTestCase
from perfkit.timeline import Timeline


class K6Runner(FakeRunner):
    """Also plays k6: writes a summary into the run folder it was given."""

    def run(self, args, cwd=None, env=None, allowed=(0,)):
        code = super().run(args, cwd, env, allowed)
        if "k6" in args:
            run_dir = env["RFMAP_PERF_RUN_DIR"]
            with open(f"{run_dir}/k6-summary.json", "w", encoding="utf-8") as out:
                json.dump(summary(), out)
        return code


class ScenarioSuiteTest(StandTestCase):
    def setUp(self):
        super().setUp()
        self.runner = K6Runner()
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(TOOL_DIR)
        self.assertEqual(problems, [])
        self.run_dir = self.root / "run"
        self.run_dir.mkdir()

    def suite(self, scenario="smoke"):
        stand = Stand(StandSettings(self.root / "data", port=self.port), self.runner, "tok")
        return ScenarioSuite(self.catalog, scenario, stand, Dataset.of({"files": {"dem.cog.tif": "abc"}}))

    def test_a_whole_scenario(self):
        suite = self.suite()
        timeline = Timeline()
        collected = suite.execute(timeline, self.run_dir)
        self.assertEqual(suite.name, "scenario/smoke")
        self.assertRegex(suite.dataset, r"^terrain-[0-9a-f]{12}$")
        self.assertEqual(suite.problems(), [])
        (entry,) = collected
        self.assertEqual(entry.gate_metrics, frozenset({"p95", "error_rate"}))
        self.assertIn("persona/link-editor", entry.involved)
        self.assertIn("fixture/warsaw-paths", entry.involved)
        names = [(e.kind, e.name) for e in timeline.events()]
        self.assertEqual(names, [("stand", "stand up"), ("setup", "set-up"), ("load", "load"), ("stage", "steady")])
        self.assertEqual(timeline.events()[3].started, "2023-11-14T22:13:20.000Z")
        plan = json.loads((self.run_dir / "plan.json").read_text(encoding="utf-8"))
        editor = next(p for p in plan["personas"] if p["id"] == "link-editor")
        self.assertEqual(len(editor["credentials"]), 8)
        self.assertEqual(plan["baseUrl"], "http://rfmap-server:8080/api/v1")
        environment = suite.environment()
        self.assertIn("afterLoad", environment["database"])
        self.assertEqual(environment["server"]["health"]["ok"], True)
        self.assertEqual((self.run_dir / "raw" / "rfmap-server.log").read_text(encoding="utf-8"), "server log\n")
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_items_and_thresholds(self):
        keys = [item.key for item in self.suite("peak").items()]
        self.assertEqual(keys[0], "scenario/peak")
        self.assertIn("collector/k6-summary", keys)
        self.runner.k6_code = 99
        suite = self.suite()
        suite.execute(Timeline(), self.run_dir)
        self.assertEqual(suite.problems(), ["scenario/smoke: k6 thresholds crossed (see k6-summary.json)"])

    def test_the_stand_goes_down_after_a_failure(self):
        with patch.object(Stand, "up", side_effect=PerfError("no stand")), \
                self.assertRaisesRegex(PerfError, "no stand"):
            self.suite().execute(Timeline(), self.run_dir)
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_unknown_scenario_and_missing_start(self):
        with self.assertRaisesRegex(PerfError, "no scenario nope"):
            self.suite("nope")
        (self.run_dir / "k6-summary.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(PerfError, "no start time"):
            self.suite()._stages(Timeline(), self.run_dir / "k6-summary.json")


if __name__ == "__main__":
    unittest.main()
