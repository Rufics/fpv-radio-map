#!/usr/bin/env python3
"""Runs the unittest tests of a folder and writes their results as JUnit XML for the CI report.

    python3 -I tools/ci/unittest_junit.py --start tools/ci --output test-results/python-ci.xml

Discovers `*_test.py` under the start folder, prints the usual unittest output, writes one
<testcase> per test with the traceback of each failure or error, and exits non-zero when a test
failed, so the CI step fails as it did with `python3 -m unittest discover`.
"""
import argparse
import sys
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


class JUnitResult(unittest.TextTestResult):
    """A text test result that also keeps every outcome for the JUnit file."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.records = []
        self._started = 0.0

    def startTest(self, test):
        self._started = time.perf_counter()
        super().startTest(test)

    def _record(self, test, outcome, kind=None, detail=""):
        self.records.append({
            "test": test,
            "outcome": outcome,
            "kind": kind,
            "detail": detail,
            "seconds": time.perf_counter() - self._started,
        })

    def addSuccess(self, test):
        super().addSuccess(test)
        self._record(test, "passed")

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._record(test, "failed", "failure", self._exc_info_to_string(err, test))

    def addError(self, test, err):
        super().addError(test, err)
        self._record(test, "failed", "error", self._exc_info_to_string(err, test))

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._record(test, "skipped", "skipped", reason)

    def addExpectedFailure(self, test, err):
        super().addExpectedFailure(test, err)
        self._record(test, "passed")

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self._record(test, "failed", "failure", "The test was expected to fail but passed.")


class JUnitWriter:
    """The JUnit XML of a run."""

    def __init__(self, suite_name):
        self.suite_name = suite_name

    def document(self, records):
        root = ET.Element("testsuites", tests=str(len(records)))
        suite = ET.SubElement(root, "testsuite", name=self.suite_name, tests=str(len(records)),
                              failures=str(self._count(records, "failure")),
                              errors=str(self._count(records, "error")),
                              skipped=str(self._count(records, "skipped")))
        for record in records:
            self._case(suite, record)
        ET.indent(root)
        return ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n"

    @staticmethod
    def _count(records, kind):
        return sum(1 for r in records if r["kind"] == kind)

    @staticmethod
    def _case(suite, record):
        test = record["test"]
        test_id = test.id()
        classname, _, name = test_id.rpartition(".")
        case = ET.SubElement(suite, "testcase", classname=classname, name=name or test_id,
                             time=f"{record['seconds']:.3f}")
        if record["kind"] == "skipped":
            ET.SubElement(case, "skipped", message=record["detail"])
        elif record["kind"]:
            lines = record["detail"].strip().splitlines()
            element = ET.SubElement(case, record["kind"], message=lines[-1] if lines else "")
            element.text = record["detail"]


class UnittestJUnitCommand:
    """The command line: discover, run, write the JUnit file."""

    PATTERN = "*_test.py"

    def __init__(self, argv=None):
        parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
        parser.add_argument("--start", required=True, type=Path, help="folder to discover tests in")
        parser.add_argument("--output", required=True, type=Path, help="JUnit XML file to write")
        parser.add_argument("--name", default="python", help="name of the test suite in the file")
        self.args = parser.parse_args(argv)

    def run(self):
        start = self.args.start.resolve()
        if not start.is_dir():
            sys.stderr.write(f"{start} is not a folder\n")
            return 2
        suite = unittest.defaultTestLoader.discover(str(start), pattern=self.PATTERN, top_level_dir=str(start))
        runner = unittest.TextTestRunner(resultclass=JUnitResult, verbosity=1)
        result = runner.run(suite)
        self._write(JUnitWriter(self.args.name).document(result.records))
        return 0 if result.wasSuccessful() and result.testsRun else 1

    def _write(self, xml):
        output = self.args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(xml, encoding="utf-8")


if __name__ == "__main__":
    sys.exit(UnittestJUnitCommand().run())
