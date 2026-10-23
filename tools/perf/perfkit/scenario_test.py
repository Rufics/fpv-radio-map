import io
import json
import shutil
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.errors import PerfError
from perfkit.google_benchmark_test import TOOL_DIR
from perfkit.k6_summary_test import summary
from perfkit.micro_test import TargetsOnly
from perfkit.profile_test import comparison
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

    def suite(self, scenario="smoke", **settings):
        settings = {"prometheus_port": self.port, "grafana_port": self.port, **settings}
        stand = Stand(StandSettings(self.root / "data", port=self.port, **settings), self.runner, "tok",
                      sleep=lambda _s: None)
        self.log = io.StringIO()
        return ScenarioSuite(self.catalog, scenario, stand, Dataset.of({"files": {"dem.cog.tif": "abc"}}),
                             log=self.log)

    def test_a_whole_scenario(self):
        suite = self.suite()
        timeline = Timeline()
        collected = suite.execute(timeline, self.run_dir)
        self.assertEqual(suite.name, "scenario/smoke")
        self.assertRegex(suite.dataset, r"^terrain-[0-9a-f]{12}$")
        self.assertEqual(suite.problems(), [])
        entry, monitored = collected
        self.assertEqual(entry.gate_metrics, frozenset({"p95", "error_rate"}))
        self.assertIn("persona/link-editor", entry.involved)
        self.assertIn("fixture/warsaw-paths", entry.involved)
        self.assertIn("collector/prometheus", entry.involved, "runs without monitoring are not compared")
        self.assertEqual(monitored.collector.key, "collector/prometheus")
        names = {(m.name, m.metric) for m in monitored.collection.measurements}
        self.assertIn(("server_cpu", "max"), names)
        self.assertIn(("pool_queued/http", "avg"), names)
        self.assertNotIn(("k6_p95", "max"), names)
        raw = json.loads((self.run_dir / "raw" / "prometheus.json").read_text(encoding="utf-8"))
        self.assertIn("k6_p95", [query["name"] for query in raw["queries"]])
        self.assertEqual(suite.environment()["extras"], ["prometheus"])
        names = [(e.kind, e.name) for e in timeline.events()]
        self.assertEqual(names, [("stand", "stand up"), ("setup", "set-up"), ("load", "load"), ("stage", "steady")])
        self.assertEqual(timeline.events()[3].started, "2023-11-14T22:13:20.000Z")
        plan = json.loads((self.run_dir / "plan.json").read_text(encoding="utf-8"))
        editor = next(p for p in plan["personas"] if p["id"] == "link-editor")
        self.assertEqual(len(editor["credentials"]), 8)
        self.assertEqual(plan["baseUrl"], "http://rfmap-server:8080/api/v1")
        environment = suite.environment()
        self.assertIn("afterLoad", environment["database"])
        self.assertTrue(environment["server"]["health"]["ok"])
        self.assertEqual((self.run_dir / "raw" / "rfmap-server.log").read_text(encoding="utf-8"), "server log\n")
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_without_monitoring_the_prometheus_collector_is_skipped(self):
        suite = self.suite(monitoring=False)
        (entry,) = suite.execute(Timeline(), self.run_dir)
        self.assertEqual(entry.collector.key, "collector/k6-summary")
        self.assertEqual(self.log.getvalue().count("skipping collector/prometheus"), 1)
        self.assertNotIn("collector/prometheus", [item.key for item in suite.items()])

    def test_a_sanitized_run_stops_the_server_before_collecting(self):
        suite = self.suite(sanitize=True, monitoring=False)
        timeline = Timeline()
        collected = suite.execute(timeline, self.run_dir)
        self.assertEqual([entry.collector.key for entry in collected], ["collector/k6-summary", "collector/sanitizer"])
        self.assertIn(("stand", "server stop"), [(e.kind, e.name) for e in timeline.events()])
        self.assertEqual(collected[1].limits, {"errors": 0.0, "leaked_bytes": 0.0, "leaked_allocations": 0.0})

    def test_a_kept_stand_gets_grafana_with_the_run_marked(self):
        suite = self.suite(keep=True)
        suite.execute(Timeline(), self.run_dir)
        stand_dir = Path(self.runner.envs[0]["RFMAP_PERF_WORK_DIR"])
        self.addCleanup(shutil.rmtree, stand_dir, True)
        self.assertTrue((stand_dir / "dashboards" / "rfmap-stand.json").is_file())
        texts = [call[2]["text"] for call in self.server.calls if call[1] == "/api/annotations"]
        self.assertEqual(texts, ["stand: stand up", "setup: set-up", "load: load", "stage: steady"])
        self.assertIn("Grafana: http://127.0.0.1:", self.log.getvalue())
        self.assertEqual(suite.problems(), [])

    def test_a_regression_profiles_the_server_under_the_peak_load(self):
        profiles = TargetsOnly()
        self.suite("peak").profile([comparison("all", 20.0)], self.run_dir, profiles)
        (target,) = profiles.targets
        self.assertEqual((target.name, target.pid, target.duration_s), ("rfmap-server", 4242, 30.0))
        plan = json.loads((self.run_dir / "profile" / "plan.json").read_text(encoding="utf-8"))
        peak = max(stage["targetRps"] for stage in self.catalog.get("scenario", "peak").manifest["load"]["stages"])
        self.assertEqual(plan["load"], {"type": "constant-arrival-rate",
                                        "stages": [{"name": "profile", "durationS": 45.0, "targetRps": peak}]})
        self.assertTrue((self.run_dir / "profile" / "k6-summary.json").is_file(), "k6 ran into profile/")
        self.assertFalse((self.run_dir / "k6-summary.json").exists())
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_no_regression_needs_no_profile(self):
        profiles = TargetsOnly()
        self.assertEqual(self.suite().profile([], self.run_dir, profiles), [])
        self.assertEqual((profiles.targets, self.runner.runs), ([], []))

    def test_a_failed_profile_load_is_an_error(self):
        self.runner.k6_code = 1
        suite, profiles, regressions = self.suite(), TargetsOnly(), [comparison("all", 20.0)]
        with self.assertRaisesRegex(PerfError, "exit 1"):
            suite.profile(regressions, self.run_dir, profiles)
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_a_failed_profile_still_waits_for_the_load(self):
        suite, profiles, regressions = self.suite(), TargetsOnly(), [comparison("all", 20.0)]
        loaded, done_at_down = threading.Event(), []

        def slow_load(_stand):
            time.sleep(0.2)
            loaded.set()
            return True

        def record_down(_stand):
            done_at_down.append(loaded.is_set())

        with patch.object(Stand, "server_pid", side_effect=PerfError("the stand's server is not running")), \
                patch.object(Stand, "load", slow_load), \
                patch.object(Stand, "down", record_down), \
                self.assertRaisesRegex(PerfError, "server is not running"):
            suite.profile(regressions, self.run_dir, profiles)
        self.assertEqual(done_at_down, [True], "the load finished before the stand went down")

    def test_items_and_thresholds(self):
        keys = [item.key for item in self.suite("peak").items()]
        self.assertEqual(keys[0], "scenario/peak")
        self.assertIn("collector/k6-summary", keys)
        self.runner.k6_code = 99
        suite = self.suite()
        suite.execute(Timeline(), self.run_dir)
        self.assertEqual(suite.problems(), ["scenario/smoke: k6 thresholds crossed (see k6-summary.json)"])

    def test_the_stand_goes_down_after_a_failure(self):
        suite, timeline = self.suite(), Timeline()
        with patch.object(Stand, "up", side_effect=PerfError("no stand")), \
                self.assertRaisesRegex(PerfError, "no stand"):
            suite.execute(timeline, self.run_dir)
        self.assertEqual(self.runner.runs[-1][-3:], ["down", "-v", "--remove-orphans"])

    def test_unknown_scenario_and_missing_start(self):
        with self.assertRaisesRegex(PerfError, "no scenario nope"):
            self.suite("nope")
        (self.run_dir / "k6-summary.json").write_text("{}", encoding="utf-8")
        suite, timeline = self.suite(), Timeline()
        with self.assertRaisesRegex(PerfError, "no start time"):
            suite._stages(timeline, self.run_dir / "k6-summary.json")


if __name__ == "__main__":
    unittest.main()
