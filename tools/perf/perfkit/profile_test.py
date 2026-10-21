import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from perfkit.catalog import CatalogLoader, KindRegistry
from perfkit.catalog_test import SCHEMAS_DIR
from perfkit.compare import Comparison, Verdict
from perfkit.environment import Environment
from perfkit.environment_test import StaticProbe
from perfkit.errors import PerfError
from perfkit.google_benchmark_test import TOOL_DIR, CatalogWithCollector
from perfkit.measure import LOWER, Measurement
from perfkit.profile import Profile, ProfileRunner, ProfileTarget
from perfkit.session import RunSession, SessionSettings
from perfkit.session_test import FakeSuite, SteppingClock

PROFILERS_DIR = TOOL_DIR / "extensions" / "profilers"


class CatalogWithProfiler(CatalogWithCollector):
    """The sample catalog with the repository's real flamegraph profiler."""

    def __init__(self, root: Path):
        super().__init__(root)
        shutil.copytree(PROFILERS_DIR, root / "extensions" / "profilers", ignore=shutil.ignore_patterns("__pycache__"))


class PerfRunner:
    """Plays perf and inferno: records the commands and writes what each would."""

    def __init__(self, fail_on: str = ""):
        self.runs: list[list[str]] = []
        self.fail_on = fail_on

    def run(self, args, cwd=None, env=None, allowed=(0,), timeout=0, stdout_path=None):
        self.runs.append(args)
        if self.fail_on in args:
            raise PerfError(f"{self.fail_on} failed")
        if "record" in args:
            Path(args[args.index("-o") + 1]).write_bytes(b"perf data")
        if stdout_path is not None:
            stdout_path.write_text(f"output of {args[1]}\n", encoding="utf-8")
        return 0


def comparison(name: str, worse_pct: float, verdict: Verdict = Verdict.REGRESSION, gated: bool = True) -> Comparison:
    measurement = Measurement("benchmark/rf-core", name, "cpu_time", "ns", LOWER, 1.0)
    return Comparison(measurement, 1.0, 5, worse_pct, 5.0, gated, verdict)


class ProfileTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        CatalogWithProfiler(self.root / "catalog")
        self.catalog, problems = CatalogLoader(KindRegistry.load(SCHEMAS_DIR)).load(self.root / "catalog")
        self.assertEqual(problems, [])
        self.item = self.catalog.get("profiler", "flamegraph")
        # Every tool is "installed": the fake runner never starts it.
        self.item.manifest["options"].update(perf=sys.executable, collapse=sys.executable, flamegraph=sys.executable)
        self.runner = PerfRunner()
        self.clock = SteppingClock()
        self.log = io.StringIO()
        self.run_dir = self.root / "run"
        self.run_dir.mkdir()

    def profile(self, target: ProfileTarget) -> Profile:
        (done,) = ProfileRunner([self.item], self.runner, self.log).profile([target], self.run_dir)
        return done


class ProfileTargetTest(unittest.TestCase):
    def test_a_command_or_a_process(self):
        for fields in ({}, {"command": ("x",), "pid": 1, "duration_s": 1}, {"pid": 7}):
            with self.subTest(fields=fields), self.assertRaises(PerfError):
                ProfileTarget("t", **fields)
        self.assertEqual(ProfileTarget("BM_Path/100 <x>", ("b",)).file_name, "BM_Path-100-x")
        self.assertEqual(ProfileTarget("///", ("b",)).file_name, "target")

    def test_regressions_worst_first(self):
        found = ProfileRunner.regressions([comparison("a", 6.0), comparison("b", 40.0),
                                           comparison("c", 50.0, Verdict.OK), comparison("d", 60.0, gated=False)])
        self.assertEqual([c.measurement.name for c in found], ["b", "a"])


class FlamegraphProfilerTest(ProfileTestCase):
    def test_a_command(self):
        done = self.profile(ProfileTarget("BM_Path/100", ("bench", "--benchmark_filter=x")))
        self.assertEqual(done, Profile("profiler/flamegraph", "BM_Path/100",
                                       ("raw/flamegraph-BM_Path-100.svg", "raw/BM_Path-100.folded")))
        record, script, collapse, draw = self.runner.runs
        self.assertEqual(Path(collapse[1]).name, "BM_Path-100.perf.txt")
        self.assertEqual(record[1:7], ["record", "-F", "99", "--call-graph", "dwarf", "-o"])
        self.assertEqual(record[-3:], ["--", "bench", "--benchmark_filter=x"])
        self.assertEqual(script[1:3], ["script", "-i"])
        self.assertEqual(draw[1:3], ["--title", "BM_Path/100"])
        self.assertEqual(sorted(path.name for path in (self.run_dir / "raw").iterdir()),
                         ["BM_Path-100.folded", "flamegraph-BM_Path-100.svg"], "perf.data and the stacks are gone")
        self.assertIn(f"profile: {self.run_dir / 'raw' / 'flamegraph-BM_Path-100.svg'}", self.log.getvalue())

    def test_a_running_process(self):
        self.profile(ProfileTarget("rfmap-server", pid=4242, duration_s=30))
        self.assertEqual(self.runner.runs[0][-5:], ["-p", "4242", "--", "sleep", "30"])

    def test_a_failure_is_noted_and_cleaned_up(self):
        self.runner.fail_on = "script"
        done = self.profile(ProfileTarget("rfmap-server", pid=1, duration_s=1))
        self.assertEqual((done.files, done.error), ((), "script failed"))
        self.assertEqual(list((self.run_dir / "raw").iterdir()), [])
        self.assertIn("could not profile rfmap-server: script failed", self.log.getvalue())

    def test_options_and_tools(self):
        cases = [({"frequencyHz": 0}, "frequencyHz must be from 1 to 10000"),
                 ({"callGraph": "magic"}, "callGraph must be one of dwarf, fp, lbr"),
                 ({"collapse": "no-such-inferno-tool"}, "no-such-inferno-tool is not installed")]
        for options, message in cases:
            with self.subTest(options=options):
                saved = dict(self.item.manifest["options"])
                self.item.manifest["options"].update(options)
                self.assertIn(message, self.profile(ProfileTarget("t", ("b",))).error)
                self.item.manifest["options"] = saved

    def test_another_perf_binary(self):
        with patch.dict(os.environ, {"RFMAP_PERF_PERF": "perf"}):
            self.assertIn("must be the absolute path", self.profile(ProfileTarget("t", ("b",))).error)
        with patch.dict(os.environ, {"RFMAP_PERF_PERF": sys.executable}):
            self.profile(ProfileTarget("t", ("b",)))
        self.assertEqual(self.runner.runs[0][0], sys.executable)


class ProfilingSuite(FakeSuite):
    def __init__(self, catalog, values, error: str = ""):
        super().__init__(catalog, values)
        self.profiled: list[list[str]] = []
        self.error = error

    def profile(self, regressions, run_dir, profiles):
        self.profiled.append([c.measurement.name for c in regressions])
        if self.error:
            raise PerfError(self.error)
        return [Profile("profiler/flamegraph", "BM_A", ("raw/flamegraph-BM_A.svg",))]


class SessionProfilesTest(ProfileTestCase):
    def run_suite(self, suite: FakeSuite) -> tuple[int, dict, list]:
        output = io.StringIO()
        environment = Environment([StaticProbe("host", {"cpuModel": "X"})])
        profiles = ProfileRunner([self.item], self.runner, output)
        session = RunSession(SessionSettings(self.root / "results"), environment, output, self.clock,
                             profiles=profiles)
        code = session.run(suite)
        run_dir = max(path for path in (self.root / "results").iterdir() if path.is_dir())
        events = json.loads((run_dir / "events.json").read_text(encoding="utf-8"))
        return code, json.loads((run_dir / "results.json").read_text(encoding="utf-8")), events

    def test_only_a_regression_is_profiled(self):
        for _ in range(3):
            suite = ProfilingSuite(self.catalog, {"BM_A": 100.0, "BM_B": 100.0})
            _, results, _ = self.run_suite(suite)
            self.assertEqual((suite.profiled, results["profiles"]), ([], []))
        suite = ProfilingSuite(self.catalog, {"BM_A": 150.0, "BM_B": 120.0})
        code, results, events = self.run_suite(suite)
        self.assertEqual(code, 1)
        self.assertEqual(suite.profiled, [["BM_A", "BM_B"]])
        self.assertEqual(results["profiles"][0]["files"], ["raw/flamegraph-BM_A.svg"])
        self.assertEqual([(e["name"], e["outcome"]) for e in events if e["kind"] == "profile"], [("profile", "ok")])

    def test_a_suite_that_cannot_profile(self):
        for _ in range(3):
            self.run_suite(ProfilingSuite(self.catalog, {"BM_A": 100.0}))
        code, results, _ = self.run_suite(ProfilingSuite(self.catalog, {"BM_A": 150.0}, error="no stand"))
        self.assertEqual(code, 1)
        self.assertEqual(results["profiles"], [{"profiler": "", "target": "micro", "files": [], "error": "no stand"}])
        self.assertEqual(results["problems"], [], "the regression fails the run, not the missing profile")


if __name__ == "__main__":
    unittest.main()
