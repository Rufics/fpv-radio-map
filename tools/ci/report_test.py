"""Tests of the CI report builder: python3 -I -m unittest discover -s tools/ci -p '*_test.py'"""
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

from report import (MARKER, build_report, changed_line_coverage, component_of, md, normalize_jobs,
                    parse_coverage, parse_diff, parse_junit, read_capped)


def gcovr(*files):
    return json.dumps({"gcovr/format_version": "0.6", "files": list(files)})


def cov_file(path, hits, fns=()):
    return {
        "file": path,
        "lines": [{"line_number": n, "count": c, "branches": []} for n, c in hits.items()],
        "functions": [{"name": name, "lineno": i + 1, "execution_count": c} for i, (name, c) in enumerate(fns)],
    }


class JunitTest(unittest.TestCase):
    def test_totals_and_failed_names(self):
        r = parse_junit('<testsuites tests="3"><testcase classname="A" name="ok"/>'
                        '<testcase classname="A" name="bad"><failure/></testcase>'
                        '<testcase name="off"><skipped/></testcase></testsuites>')
        self.assertEqual(r, {"total": 3, "failed": ["A › bad"], "skipped": 1, "passed": 1})


class CoverageTest(unittest.TestCase):
    def test_paths_are_grouped_into_areas(self):
        self.assertEqual(component_of("core/rf-core/src/a.cpp"), ("Core", "rf-core"))
        self.assertEqual(component_of("core/rf-ext/src/a.cpp"), ("Core", "rf-ext"))
        self.assertEqual(component_of("modules/fpv/relay/src/relay.cpp"), ("FPV modules", "fpv.relay"))
        self.assertEqual(component_of("modules/fpv/common/include/fpv/world.hpp"), ("FPV modules", "fpv common"))
        self.assertEqual(component_of("platform/rfmod/src/registry.cpp"), ("Platform", "rfmod"))
        self.assertEqual(component_of("server/src/app.cpp"), ("Server", "server"))
        self.assertIsNone(component_of("examples/modules/x.cpp"))

    def test_each_line_counts_once_and_unsafe_paths_are_dropped(self):
        files = parse_coverage(gcovr(
            cov_file("core/rf-core/src/a.cpp", {1: 1, 2: 0}, [("f", 1), ("g", 0)]),
            {"file": "core/rf-core/src/a.cpp", "lines": [{"line_number": 2, "count": 3}], "functions": []},
            cov_file("../etc/passwd", {1: 1}),
            cov_file("server/src/<b>.cpp", {1: 1}),
        ))
        self.assertEqual(list(files), ["core/rf-core/src/a.cpp"])
        a = files["core/rf-core/src/a.cpp"]
        self.assertEqual((a["lines"], a["linesCovered"], a["functions"], a["functionsCovered"]), (2, 2, 2, 1))
        self.assertEqual(parse_coverage("not json"), {})


class DiffTest(unittest.TestCase):
    def test_changed_lines_come_from_the_new_side(self):
        diff = "\n".join([
            "diff --git a/core/rf-core/src/a.cpp b/core/rf-core/src/a.cpp",
            "--- a/core/rf-core/src/a.cpp",
            "+++ b/core/rf-core/src/a.cpp",
            "@@ -1,0 +2,3 @@",
            "+x",
            "@@ -9 +10 @@",
            "+y",
            "diff --git a/gone.cpp b/gone.cpp",
            "+++ /dev/null",
            "@@ -1,2 +0,0 @@",
        ])
        changed = parse_diff(diff)
        self.assertEqual(changed, {"core/rf-core/src/a.cpp": {2, 3, 4, 10}})
        files = parse_coverage(gcovr(cov_file("core/rf-core/src/a.cpp", {2: 1, 3: 0, 4: 0, 10: 5, 11: 0})))
        c = changed_line_coverage(files, changed)
        self.assertEqual((c["total"], c["covered"]), (4, 2))
        self.assertEqual(c["uncovered"], [{"path": "core/rf-core/src/a.cpp", "from": 3, "to": 4}])


class JobsTest(unittest.TestCase):
    def test_needs_context_or_actions_api(self):
        self.assertEqual(normalize_jobs({"changes": {"result": "success"}, "cpp": {"result": "failure"}}),
                         [{"name": "C++ build and tests", "result": "failure", "url": None}])
        self.assertEqual(normalize_jobs([
            {"name": "Test report", "conclusion": "success"},
            {"name": "Web build and tests", "conclusion": "success", "url": "https://github.com/o/r/actions/runs/1/job/2"},
            {"name": "Evil", "conclusion": "success", "url": "javascript:alert(1)"},
        ]), [
            {"name": "Web build and tests", "result": "success", "url": "https://github.com/o/r/actions/runs/1/job/2"},
            {"name": "Evil", "result": "success", "url": None},
        ])


class EscapingTest(unittest.TestCase):
    def test_untrusted_text_cannot_inject_markdown_html_or_mentions(self):
        s = md("<img src=x> [link](http://evil) @maintainer | `code`")
        self.assertNotIn("<img", s)
        self.assertNotIn("](http", s)
        self.assertNotIn("@maintainer", s)
        self.assertIsNone(re.search(r"[^\\]\|", s))


class InputTest(unittest.TestCase):
    def test_reads_stay_inside_the_current_directory(self):
        old = Path.cwd()
        with tempfile.TemporaryDirectory() as outer:
            inner = Path(outer) / "data"
            inner.mkdir()
            (Path(outer) / "secret.txt").write_text("secret")
            (inner / "ok.txt").write_text("ok")
            os.chdir(inner)
            try:
                self.assertEqual(read_capped("ok.txt"), "ok")
                self.assertIsNone(read_capped("../secret.txt"))
                self.assertIsNone(read_capped("missing.txt"))
            finally:
                os.chdir(old)


class ReportTest(unittest.TestCase):
    def test_passing_run_with_coverage(self):
        coverage = parse_coverage(gcovr(
            cov_file("core/rf-core/src/a.cpp", {1: 1, 2: 1, 3: 1, 4: 0}),
            cov_file("modules/fpv/relay/src/r.cpp", {1: 1, 2: 0}),
        ))
        out = build_report(
            jobs={"changes": {"result": "success"}, "cpp": {"result": "success"},
                  "coverage": {"result": "success"}, "web": {"result": "skipped"}},
            suites=[{"file": "cpp.xml", "label": "C++", "total": 5, "passed": 5, "skipped": 0, "failed": []}],
            coverage=coverage,
            diff=parse_diff("+++ b/core/rf-core/src/a.cpp\n@@ -0,0 +3,2 @@\n"),
            sha="0123456789abcdef",
            commit="fedcba9876543210",
            repo="o/r",
        )
        self.assertTrue(out.startswith(f"{MARKER}\n### ✅ CI passed for 0123456"))
        self.assertIn("**5** tests passed · C++ line coverage **66.7%** · changed lines **50.0%** covered", out)
        self.assertIn("| Core: rf-core, rf-ext | 🟡 75.0% <sub>3/4</sub>", out)
        self.assertIn("| FPV modules | 🔴 50.0% <sub>1/2</sub>", out)
        self.assertIn("`core/rf-core/src/a.cpp`: [4](https://github.com/o/r/blob/fedcba9876543210/core/rf-core/src/a.cpp#L4)", out)
        self.assertIn("| Web build and tests | ⏭️ skipped | not affected by this change |", out)

    def test_failing_run_leads_with_the_failed_tests(self):
        out = build_report(
            jobs={"cpp": {"result": "failure"}, "web": {"result": "success"}},
            suites=[{"file": "cpp.xml", "label": "C++", "total": 3, "passed": 2, "skipped": 0,
                     "failed": ["Geo › <b>Breaks</b>"]}],
        )
        self.assertRegex(out, r"(?m)^### ❌ CI failed: 1 test failed$")
        self.assertIn("- C++: Geo › &lt;b&gt;Breaks&lt;/b&gt;", out)

    def test_build_failing_before_its_tests_says_so(self):
        out = build_report(jobs={"cpp": {"result": "failure"}})
        self.assertRegex(out, r"(?m)^### ❌ CI failed: C\+\+ build and tests failed$")
        self.assertIn("failed before any test result was written", out)

    def test_docs_only_change_builds_nothing(self):
        out = build_report(jobs={"changes": {"result": "success"}, "cpp": {"result": "skipped"},
                                 "web": {"result": "skipped"}})
        self.assertIn("Nothing to build", out)
        self.assertNotIn("| Check |", out)


if __name__ == "__main__":
    unittest.main()
