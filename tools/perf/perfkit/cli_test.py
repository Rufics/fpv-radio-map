import io
import tempfile
import unittest
from pathlib import Path

from perfkit import cli
from perfkit.catalog_test import SampleCatalog


class PerfCliTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.sample = SampleCatalog(self.root)

    def run_cli(self, *args):
        output, errors = io.StringIO(), io.StringIO()
        code = cli.PerfCli(output=output, errors=errors).main(["--catalog-dir", str(self.root), *args])
        return code, output.getvalue(), errors.getvalue()

    def test_list_all_and_one_kind(self):
        code, output, _ = self.run_cli("list")
        self.assertEqual(code, 0)
        self.assertIn("benchmark  rf-core", output)
        self.assertIn("extensions/collectors/k6-summary", output)
        code, output, _ = self.run_cli("list", "--kind", "persona")
        self.assertEqual(code, 0)
        self.assertEqual(len(output.splitlines()), 2)

    def test_list_unknown_kind(self):
        code, _, errors = self.run_cli("list", "--kind", "plugin")
        self.assertEqual(code, 2)
        self.assertIn("known kinds:", errors)

    def test_lock_then_check(self):
        self.assertEqual(self.run_cli("catalog", "check")[0], 1)
        code, output, _ = self.run_cli("catalog", "lock")
        self.assertEqual((code, output.strip()), (0, "locked 7 item(s) in catalog.lock.json"))
        code, output, _ = self.run_cli("catalog", "check")
        self.assertEqual((code, output.strip()), (0, "catalog is consistent: 7 item(s)"))

    def test_problems_block_the_lock(self):
        self.run_cli("catalog", "lock")
        (self.root / "fixtures/link-setups/control.json").write_text('{"changed": 1}', encoding="utf-8")
        before = (self.root / "catalog.lock.json").read_text(encoding="utf-8")
        code, _, errors = self.run_cli("catalog", "lock")
        self.assertEqual(code, 1)
        self.assertIn("fixtures/link-setups: content changed but version stayed 1.0.0", errors)
        self.assertIn("1 problem(s) in the catalog", errors)
        self.assertEqual((self.root / "catalog.lock.json").read_text(encoding="utf-8"), before)

    def test_missing_catalog(self):
        code, _, errors = self.run_cli("--catalog-dir", str(self.root / "nope"), "list")
        self.assertEqual(code, 1)
        self.assertIn("does not exist", errors)

    def test_run_stops_before_building(self):
        results = ["--results-dir", str(self.root / "results")]
        code, _, errors = self.run_cli("run", "--suite", "micro", "--repetitions", "0", *results)
        self.assertEqual((code, errors.strip()), (2, "error: --repetitions must be at least 1"))
        code, _, errors = self.run_cli("run", "--suite", "micro", "--only", "nope", *results)
        self.assertEqual(code, 1)
        self.assertIn("no benchmark nope", errors)
        (self.root / "personas" / "empty").mkdir()
        code, _, errors = self.run_cli("run", "--suite", "micro", *results)
        self.assertEqual(code, 1)
        self.assertIn("personas/empty: no manifest.json", errors)
        self.assertFalse((self.root / "results").exists())

    def test_scenario_needs_a_checked_dataset(self):
        code, _, errors = self.run_cli("run", "--scenario", "peak")
        self.assertEqual(code, 1)
        self.assertIn("--scenario needs --dataset", errors)
        code, _, errors = self.run_cli("run", "--scenario", "peak", "--dataset", str(self.root / "none"))
        self.assertEqual(code, 1)
        self.assertIn("perf-dataset.json is missing", errors)

    def test_repository_catalog_is_consistent(self):
        output, errors = io.StringIO(), io.StringIO()
        self.assertEqual(cli.PerfCli(output=output, errors=errors).main(["catalog", "check"]), 0, errors.getvalue())


if __name__ == "__main__":
    unittest.main()
