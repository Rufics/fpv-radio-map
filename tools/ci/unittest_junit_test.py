"""Tests of the unittest JUnit runner: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from junit import JUnitParser
from unittest_junit import UnittestJUnitCommand

SAMPLE = '''import unittest


class Sample(unittest.TestCase):
    def test_passes(self):
        self.assertTrue(True)

    def test_fails(self):
        self.assertEqual(1, 2, "one is not <two>")

    def test_errors(self):
        raise KeyError("missing")

    @unittest.skip("not here")
    def test_skipped(self):
        pass

    @unittest.expectedFailure
    def test_expected_failure(self):
        self.fail()

    @unittest.expectedFailure
    def test_unexpected_success(self):
        pass
'''


class CommandTest(unittest.TestCase):
    def run_command(self, folder, output):
        command = UnittestJUnitCommand(["--start", str(folder), "--output", str(output), "--name", "sample"])
        with contextlib.redirect_stderr(io.StringIO()):
            return command.run()

    def test_writes_every_outcome_and_fails_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "suite"
            folder.mkdir()
            (folder / "junit_sample_case_test.py").write_text(SAMPLE)
            output = Path(tmp) / "out" / "python.xml"
            self.assertEqual(self.run_command(folder, output), 1)
            xml = output.read_text()
        self.assertIn('<testsuite name="sample" tests="6" failures="2" errors="1" skipped="1">', xml)
        cases = {c.name.rpartition(" › ")[2]: c for c in JUnitParser().parse(xml).cases}
        self.assertEqual({n: c.outcome for n, c in cases.items()}, {
            "test_passes": "passed", "test_fails": "failed", "test_errors": "failed", "test_skipped": "skipped",
            "test_expected_failure": "passed", "test_unexpected_success": "failed",
        })
        self.assertIn("one is not <two>", cases["test_fails"].reason)
        self.assertEqual(cases["test_fails"].reason.count("AssertionError"), 1)
        self.assertIn("KeyError: 'missing'", cases["test_errors"].reason)

    def test_missing_folder_or_no_tests_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out.xml"
            self.assertEqual(self.run_command(Path(tmp) / "missing", output), 2)
            self.assertEqual(self.run_command(Path(tmp), output), 1)
            self.assertIn('tests="0"', output.read_text())


if __name__ == "__main__":
    unittest.main()
